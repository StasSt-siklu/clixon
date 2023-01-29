/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****
  Commit-confirm
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <pwd.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_backend_transaction.h"
#include "clixon_backend_plugin.h"
#include "backend_handle.h"
#include "clixon_backend_commit.h"
#include "backend_client.h"

/* 
 * Local types 
 */
/* A struct to store the information necessary for tracking the status and relevant details of
 * one or more overlapping confirmed-commit events.
 */
struct confirmed_commit {
    enum confirmed_commit_state cc_state;
    char       *cc_persist_id;       /* a value given by a client in the confirmed-commit */
    uint32_t    cc_session_id;       /* the session_id of the client that gave no <persist> value */
    int        (*cc_fn)(int, void*); /* function pointer for rollback event (rollback_fn()) */
    void        *cc_arg;             /* clicon_handle that will be passed to rollback_fn() */
};

int
confirmed_commit_init(clicon_handle h)
{
    int                      retval = -1;
    struct confirmed_commit *cc = NULL;

    if ((cc = calloc(1, sizeof(*cc))) == NULL){
        clicon_err(OE_UNIX, errno, "calloc");
        goto done;
    }
    cc->cc_state = INACTIVE;
    if (clicon_ptr_set(h, "confirmed-commit-struct", cc) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! If confirm commit persist-id exists, free it
 * @param[in] h  Clixon handle
 * @retval    0  OK
 */
int
confirmed_commit_free(clicon_handle h)
{
    struct confirmed_commit *cc = NULL;
    
    clicon_ptr_get(h, "confirmed-commit-struct", (void**)&cc);
    if (cc != NULL){
        if (cc->cc_persist_id != NULL)
            free (cc->cc_persist_id);
        free(cc);
    }
    clicon_ptr_del(h, "confirmed-commit-struct");
    return 0;
}

/*
 * Accessor functions
 */
enum confirmed_commit_state
confirmed_commit_state_get(clicon_handle h)
{
    struct confirmed_commit *cc = NULL;
    
    clicon_ptr_get(h, "confirmed-commit-struct", (void**)&cc);
    return cc->cc_state;
}

static int
confirmed_commit_state_set(clicon_handle h,
                           enum confirmed_commit_state state)
{
    struct confirmed_commit *cc = NULL;
    
    clicon_ptr_get(h, "confirmed-commit-struct", (void**)&cc);
    cc->cc_state = state;
    return 0;
}

char *
confirmed_commit_persist_id_get(clicon_handle h)
{
    struct confirmed_commit *cc = NULL;
    
    clicon_ptr_get(h, "confirmed-commit-struct", (void**)&cc);
    return cc->cc_persist_id;
}

static int
confirmed_commit_persist_id_set(clicon_handle h,
                                char         *persist_id)
{
    struct confirmed_commit *cc = NULL;
    
    clicon_ptr_get(h, "confirmed-commit-struct", (void**)&cc);
    if (cc->cc_persist_id)
        free(cc->cc_persist_id);
    if (persist_id){
        if ((cc->cc_persist_id = strdup4(persist_id)) == NULL){
            clicon_err(OE_UNIX, errno, "strdup4");
            return -1;
        }
    }
    else
        cc->cc_persist_id = NULL;
    return 0;
}

uint32_t
confirmed_commit_session_id_get(clicon_handle h)
{
    struct confirmed_commit *cc = NULL;
    
    clicon_ptr_get(h, "confirmed-commit-struct", (void**)&cc);
    return cc->cc_session_id;
}

static int
confirmed_commit_session_id_set(clicon_handle h,
                                uint32_t      session_id)
{
    struct confirmed_commit *cc = NULL;
    
    clicon_ptr_get(h, "confirmed-commit-struct", (void**)&cc);
    cc->cc_session_id = session_id;
    return 0;
}

static int
confirmed_commit_fn_arg_get(clicon_handle h,
                            int        (**fn)(int, void*),
                            void        **arg)
{
    struct confirmed_commit *cc = NULL;
    
    clicon_ptr_get(h, "confirmed-commit-struct", (void**)&cc);
    *fn = cc->cc_fn;
    *arg = cc->cc_arg;
    return 0;
}

static int
confirmed_commit_fn_arg_set(clicon_handle h,
                            int        (*fn)(int, void*),
                            void        *arg)
{
    struct confirmed_commit *cc = NULL;
    
    clicon_ptr_get(h, "confirmed-commit-struct", (void**)&cc);
    cc->cc_fn = fn;
    cc->cc_arg = arg;
    return 0;
}

/*! Return if confirmed tag found
 * @param[in]  xe  Commit rpc xml
 * @retval     1   Confirmed tag exists
 * @retval     0   Confirmed tag does not exist
 */
static int
xe_confirmed(cxobj *xe)
{
    return (xml_find_type(xe, NULL, "confirmed", CX_ELMNT) != NULL) ? 1 : 0;
}

/*! Return if persist exists and its string value field
 * @param[in]  xe  Commit rpc xml
 * @param[out] str Pointer to persist
 * @retval     1   Persist field exists
 * @retval     0   Persist field  does not exist
 */
static int
xe_persist(cxobj *xe,
           char **str)
{
    cxobj *xml;

    if ((xml = xml_find_type(xe, NULL, "persist", CX_ELMNT)) != NULL){
        *str = xml_body(xml);
        return 1;
    }
    *str = NULL;
    return 0;
}

/*! Return if persist-id exists and its string value
 *
 * @param[in]  xe  Commit rpc xml
 * @param[out] str Pointer to persist-id
 * @retval     1   Persist-id exists
 * @retval     0   Persist-id does not exist
 */
static int
xe_persist_id(cxobj *xe,
              char **str)
{
    cxobj *xml;

    if ((xml = xml_find_type(xe, NULL, "persist-id", CX_ELMNT)) != NULL){
        *str = xml_body(xml);
        return 1;
    }
    *str = NULL;
    return 0;
}

/*! Return timeout
 * @param[in]  xe  Commit rpc xml
 * @retval     sec Timeout in seconds, can be 0 if no timeout exists or is zero
 */
static unsigned int
xe_timeout(cxobj *xe)
{
    cxobj *xml;
    char  *str;
    
    if ((xml = xml_find_type(xe, NULL, "confirm-timeout", CX_ELMNT)) != NULL &&
        (str = xml_body(xml)) != NULL)
        return strtoul(str, NULL, 10);
    return 0;
}

/*! Cancel a scheduled rollback as previously registered by schedule_rollback_event()
 *
 * @param[in]   h       Clixon handle
 * @retval      0       Rollback event successfully cancelled
 * @retval      -1      No Rollback event was found
 */
int
cancel_rollback_event(clicon_handle h)
{
    int   retval;
    int (*fn)(int, void*) = NULL;
    void *arg = NULL;

    confirmed_commit_fn_arg_get(h, &fn, &arg);
    if ((retval = clixon_event_unreg_timeout(fn, arg)) == 0) {
        clicon_log(LOG_INFO, "a scheduled rollback event has been cancelled");
    } else {
        clicon_log(LOG_WARNING, "the specified scheduled rollback event was not found");
    }

    return retval;
}

/*! Apply the rollback configuration upon expiration of the confirm-timeout
 *
 * @param[in]   fd      a dummy argument per the event callback semantics
 * @param[in]   arg     a void pointer to a clicon_handle
 * @retval      0       the rollback was successful
 * @retval      -1      the rollback failed
 * @see                 do_rollback()
 */
static int
rollback_fn(int  fd,
            void *arg)
{
    clicon_handle h = arg;

    clicon_log(LOG_CRIT, "a confirming-commit was not received before the confirm-timeout expired; rolling back");

    return do_rollback(h, NULL);
}

/*! Schedule a rollback in case no confirming-commit is received before the confirm-timeout
 *
 * @param[in]   h       a clicon handle
 * @param[in]   timeout a uint32 representing the number of seconds before the rollback event should fire
 *
 * @retval      0       Rollback event successfully scheduled
 * @retval      -1      Rollback event was not scheduled
 */
static int
schedule_rollback_event(clicon_handle h,
                        uint32_t      timeout)
{
    int retval = -1;

    // register a new scheduled event
    struct timeval t, t1;
    if (gettimeofday(&t, NULL) < 0) {
        clicon_err(OE_UNIX, 0, "failed to get time of day: %s", strerror(errno));
        goto done;
    };
    t1.tv_sec = timeout; t1.tv_usec = 0;
    timeradd(&t, &t1, &t);

    /* The confirmed-commit is either:
     * - ephemeral, and the client requesting the new confirmed-commit is on the same session, OR
     * - persistent, and the client provided the persist-id in the new confirmed-commit
     */

    /* remember the function pointer and args so the confirming-commit can cancel the rollback */
    confirmed_commit_fn_arg_set(h, rollback_fn, h);
    if (clixon_event_reg_timeout(t, rollback_fn, h, "rollback after timeout") < 0) {
        /* error is logged in called function */
        goto done;
    };

    retval = 0;

    done:
    return retval;
}

/*! Cancel a confirming commit by removing rollback, and free state
 * @param[in]  h
 * @param[out] cbret
 * @retval     0      OK
 */
int
cancel_confirmed_commit(clicon_handle h)
{
    cancel_rollback_event(h);

    if (confirmed_commit_state_get(h) == PERSISTENT &&
        confirmed_commit_persist_id_get(h) != NULL) {
        confirmed_commit_persist_id_set(h, NULL);
    }

    confirmed_commit_state_set(h, INACTIVE);

    if (xmldb_delete(h, "rollback") < 0)
        clicon_err(OE_DB, 0, "Error deleting the rollback configuration");
    return 0;
}

/*! Determine if the present commit RPC invocation constitutes a valid "confirming-commit".
 *
 * To be considered a valid confirming-commit, the <commit/> must either:
 *   1) be presented without a <persist-id> value, and on the same session as a prior confirmed-commit that itself was
 *      without a <persist> value, OR
 *   2) be presented with a <persist-id> value that matches the <persist> value accompanying the prior confirmed-commit
 *
 * @param[in]   h       Clicon handle
 * @param[in]   xe      Request: <rpc><xn></rpc> 
 * @param[in]   myid    current client session-id
 * @retval      1       The confirming-commit is valid
 * @retval      0       The confirming-commit is not valid
 * @retval      -1      Error
 */
static int
check_valid_confirming_commit(clicon_handle h,
                              cxobj        *xe,
                              uint32_t      myid)
{
    int retval = -1;
    char *persist_id = NULL;

    if (xe == NULL){
        clicon_err(OE_CFG, EINVAL, "xe is NULL");
        goto done;
    }
    if (myid == 0)
        goto invalid;
    switch (confirmed_commit_state_get(h)) {
        case PERSISTENT:
            if (xe_persist_id(xe, &persist_id)) {
                if (clicon_strcmp(persist_id, confirmed_commit_persist_id_get(h)) == 0) {
                    /* the RPC included a <persist-id> matching the prior confirming-commit's <persist> */
                    break; // valid
                }
                else {
                    clicon_log(LOG_INFO,
                               "a persistent confirmed-commit is in progress but the client issued a "
                               "confirming-commit with an incorrect persist-id");
                    goto invalid;
                }
            } else {
                clicon_log(LOG_INFO,
                           "a persistent confirmed-commit is in progress but the client issued a confirming-commit"
                           "without a persist-id");
                goto invalid;
            }
        case EPHEMERAL:
            if (myid == confirmed_commit_session_id_get(h)) {
                /* the RPC lacked a <persist-id>, the prior confirming-commit lacked <persist>, and both were issued
                 * on the same session.
                 */
                break; // valid
            }
            clicon_log(LOG_DEBUG, "an ephemeral confirmed-commit is in progress, but there confirming-commit was"
                                  "not issued on the same session as the confirmed-commit");
            goto invalid;
        default:
            clicon_debug(1, "commit-confirmed state !? %d", confirmed_commit_state_get(h));
            goto invalid;
    }
    retval = 1; // valid
 done:
    return retval;
 invalid:
    retval = 0;
    goto done;
}

/*! Handle the second phase of confirmed-commit processing.
 *
 * In the first phase, the proper action was taken in the case of a valid confirming-commit, but no subsequent
 * confirmed-commit.
 *
 * In the second phase, the action taken is to handle both confirming- and confirmed-commit by creating the
 * rollback database as required, then deleting it once the sequence is complete.
 *
 * @param[in]   h          Clicon handle
 * @param[in]   xe         Commit rpc xml or NULL
 * @param[in]   myid       Current session-id, only valid > 0 if call is made as a result of an incoming message
 * @retval      0          OK
 * @retval      -1         Error
 * @note There are some calls to this function where myid is 0 (which is invalid). It is unclear if such calls
 *       actually occur, and if so, if they are correctly handled. The calls are from do_rollback() and load_failsafe()
 */
int
handle_confirmed_commit(clicon_handle h,
                        cxobj        *xe,
                        uint32_t      myid)
{
    int           retval = -1;
    char         *persist;
    unsigned long confirm_timeout = 0L;
    int           cc_valid;
    int           db_exists;

    if (xe == NULL){
        clicon_err(OE_CFG, EINVAL, "xe is NULL");
        goto done;
    }
    if (myid == 0)
        goto ok;
    /* The case of a valid confirming-commit is also handled in the first phase, but only if there is no subsequent
     * confirmed-commit.  It is tested again here as the case of a valid confirming-commit *with* a subsequent
     * confirmed-commit must be handled once the transaction has begun and after all the plugins' validate callbacks
     * have been called.
     */
    cc_valid = check_valid_confirming_commit(h, xe, myid);
    if (cc_valid) {
        if (cancel_rollback_event(h) < 0) {
            clicon_err(OE_DAEMON, 0, "A valid confirming-commit was received, but the corresponding rollback event was not found");
        }

        if (confirmed_commit_state_get(h) == PERSISTENT &&
            confirmed_commit_persist_id_get(h) != NULL) {
            confirmed_commit_persist_id_set(h, NULL);
        }

        confirmed_commit_state_set(h, INACTIVE);
    }

    /* Now, determine if there is a subsequent confirmed-commit */
    if (xe_confirmed(xe)){
        /* There is, get it's confirm-timeout value, which will default per the yang schema if not client-specified */
        /* Clixon also pre-validates input according to the schema, so bounds checking here is redundant */
        confirm_timeout = xe_timeout(xe);
        if (xe_persist(xe, &persist)){
            if (persist == NULL) {
                confirmed_commit_persist_id_set(h, NULL);
            }
            else if (confirmed_commit_persist_id_set(h, persist) < 0){
                goto done;
            }

            /* The client has passed <persist>; the confirming-commit MUST now be accompanied by a matching
             * <persist-id>
             */
            confirmed_commit_state_set(h, PERSISTENT);
            clicon_log(LOG_INFO,
                       "a persistent confirmed-commit has been requested with persist id of '%s' and a timeout of %lu seconds",
                       confirmed_commit_persist_id_get(h), confirm_timeout);
        }

        else {
            /* The client did not pass a value for <persist> and therefore any subsequent confirming-commit must be
             * issued within the same session.
             */
            confirmed_commit_session_id_set(h, myid);
            confirmed_commit_state_set(h, EPHEMERAL);

            clicon_log(LOG_INFO,
                       "an ephemeral confirmed-commit has been requested by session-id %u and a timeout of %lu seconds",
                       confirmed_commit_session_id_get(h),
                       confirm_timeout);
        }

        /* The confirmed-commits and confirming-commits can overlap; the rollback database is created at the beginning
         * of such a sequence and deleted at the end; hence its absence implies this is the first of a sequence. **
         *
         *
         * |    edit
         * |    | confirmed-commit
         * |    | copy t=0 running to rollback
         * |    | | edit
         * |    | | | both
         * |    | | | | edit
         * |    | | | | | both
         * |    | | | | | | confirming-commit
         * |    | | | | | | | delete rollback
         * +----|-|-|-|-|-|-|-|---------------
         * t=0  1 2 3 4 5 6 7 8
         *
         * edit = edit of the candidate configuration
         * both = both a confirmed-commit and confirming-commit in the same RPC
         *
         * As shown, the rollback database created at t=2 is comprised of the running database from t=0
         * Thus, if there is a rollback event at t=7, the t=0 configuration will be committed.
         *
         *  ** the rollback database may be present at system startup if there was a crash during a confirmed-commit;
         *     in the case the system is configured to startup from running and the rollback database is present, the
         *     rollback database will be committed to running and then deleted.  If the system is configured to use a
         *     startup configuration instead, any present rollback database will be deleted.
         *
         */

        db_exists = xmldb_exists(h, "rollback");
        if (db_exists == -1) {
            clicon_err(OE_DAEMON, 0, "there was an error while checking existence of the rollback database");
            goto done;
        } else if (db_exists == 0) {
            // db does not yet exists
            if (xmldb_copy(h, "running", "rollback") < 0) {
                clicon_err(OE_DAEMON, 0, "there was an error while copying the running configuration to rollback database.");
                goto done;
            };
        }

        if (schedule_rollback_event(h, confirm_timeout) < 0) {
            clicon_err(OE_DAEMON, 0, "the rollback event could not be scheduled");
            goto done;
        };

    }
    else {
        /* There was no subsequent confirmed-commit, meaning this is the end of the confirmed/confirming sequence;
         * The new configuration is already committed to running and the rollback database can now be deleted
         */
        if (xmldb_delete(h, "rollback") < 0) {
            clicon_err(OE_DB, 0, "Error deleting the rollback configuration");
            goto done;
        }
    }
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Do a rollback of the running configuration to the state prior to initiation of a confirmed-commit
 *
 * The "running" configuration prior to the first confirmed-commit was stored in another database named "rollback".
 * Here, it is committed as if it is the candidate configuration.
 *
 * Execution has arrived here because do_rollback() was called by one of:
 *  1. backend_client_rm()          (client disconnected and confirmed-commit is ephemeral)
 *  2. from_client_cancel_commit()  (invoked either by netconf client, or CLI)
 *  3. rollback_fn()                (invoked by expiration of the rollback event timer)
 *
 * @param[in]   h       Clicon handle
 * @retval      -1      Error
 * @retval      0       Success
 * @see                 backend_client_rm()
 * @see                 from_client_cancel_commit()
 * @see                 rollback_fn()
 */
int
do_rollback(clicon_handle h,
            uint8_t      *errs)
{
    int     retval = -1;
    uint8_t errstate = 0;
    cbuf   *cbret;

    if ((cbret = cbuf_new()) == NULL) {
        clicon_err(OE_DAEMON, 0, "rollback was not performed. (cbuf_new: %s)", strerror(errno));
        /* the rollback_db won't be deleted, so one can try recovery by:
         *   load rollback running
         *   restart the backend, which will try to load the rollback_db, and delete it if successful
         *     (otherwise it will load the failsafe)
         */
        clicon_log(LOG_CRIT, "An error occurred during rollback and the rollback_db wasn't deleted.");
        errstate |= ROLLBACK_NOT_APPLIED | ROLLBACK_DB_NOT_DELETED;
        goto done;
    }

    if (confirmed_commit_state_get(h) == PERSISTENT &&
        confirmed_commit_persist_id_get(h) != NULL) {
        confirmed_commit_persist_id_set(h, NULL);
    }
    confirmed_commit_state_set(h, ROLLBACK);
    if (candidate_commit(h, NULL, "rollback", 0, VL_FULL, cbret) < 0) { /* Assume validation fail, nofatal */
        /* theoretically, this should never error, since the rollback database was previously active and therefore
         * had itself been previously and successfully committed.
         */
        clicon_log(LOG_CRIT, "An error occurred committing the rollback database.");
        errstate |= ROLLBACK_NOT_APPLIED;

        /* Rename the errored rollback database */
        if (xmldb_rename(h, "rollback", NULL, ".error") < 0) {
            clicon_log(LOG_CRIT, "An error occurred renaming the rollback database.");
            errstate |= ROLLBACK_DB_NOT_DELETED;
        }

        /* Attempt to load the failsafe config */

        if (load_failsafe(h, "Rollback") < 0) {
            clicon_log(LOG_CRIT, "An error occurred committing the failsafe database.  Exiting.");
            /* Invoke our own signal handler to exit */
            raise(SIGINT);

            /* should never make it here */
        }

        errstate |= ROLLBACK_FAILSAFE_APPLIED;
        goto done;
    }
    cbuf_free(cbret);

    if (xmldb_delete(h, "rollback") < 0) {
        clicon_log(LOG_WARNING, "A rollback occurred but the rollback_db wasn't deleted.");
        errstate |= ROLLBACK_DB_NOT_DELETED;
        goto done;
    };
    retval = 0;
 done:
    confirmed_commit_state_set(h, INACTIVE);
    if (errs)
        *errs = errstate;
    return retval;
}

/*! Cancel an ongoing confirmed commit.
 * If the confirmed commit is persistent, the parameter 'persist-id' must be
 * given, and it must match the value of the 'persist' parameter.
 * If the confirmed-commit is ephemeral, the 'persist-id' must not be given and both the confirmed-commit and the
 * cancel-commit must originate from the same session.
 *
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK. This may indicate both ok and err msg back to client
 * @retval    -1       Error
 * @see RFC 6241 Sec 8.4
 */
int
from_client_cancel_commit(clicon_handle h,
                          cxobj        *xe,
                          cbuf         *cbret,
                          void         *arg,
                          void         *regarg)
{
    cxobj               *persist_id_xml;
    char                *persist_id = NULL;
    uint32_t             myid;
    int                  retval = -1;
    int                  rollback = 0;
    struct client_entry *ce = (struct client_entry *)arg;

    myid = ce->ce_id;
    if ((persist_id_xml = xml_find_type(xe, NULL, "persist-id", CX_ELMNT)) != NULL) {
        /* persist == persist_id == NULL is legal */
        persist_id = xml_body(persist_id_xml);
    }
    switch(confirmed_commit_state_get(h)) {
        case EPHEMERAL:
            if (persist_id_xml != NULL) {
                if (netconf_invalid_value(cbret, "protocol", "current confirmed-commit is not persistent") < 0)
                    goto done;
            }
            else if (myid != confirmed_commit_session_id_get(h)) {
                if (netconf_invalid_value(cbret, "protocol", "confirming-commit must be given within session that gave the confirmed-commit") < 0)
                    goto done;
            }
            else
                rollback++;
            break;
        case PERSISTENT:
            if (persist_id_xml == NULL) {
                if (netconf_invalid_value(cbret, "protocol", "persist-id is required") < 0)
                    goto done;
            }
            else if (clicon_strcmp(persist_id, confirmed_commit_persist_id_get(h)) != 0){
                if (netconf_invalid_value(cbret, "application", "a confirmed-commit with the given persist-id was not found") < 0)
                    goto done;
            }
            else
                rollback++;
            break;
        case INACTIVE:
            if (netconf_invalid_value(cbret, "application", "no confirmed-commit is in progress") < 0)
                goto done;
            break;
        default:
            if (netconf_invalid_value(cbret, "application", "server error") < 0)
                goto done;
            break;
    }
    /* all invalid conditions jump to done: and valid code paths jump to or fall through to here. */
    if (rollback){
        cancel_rollback_event(h);
        if (do_rollback(h, NULL) < 0)
            goto done;
        cprintf(cbret, "<rpc-reply xmlns=\"%s\"><ok/></rpc-reply>", NETCONF_BASE_NAMESPACE);
        clicon_log(LOG_INFO, "a confirmed-commit has been cancelled by client request");
    }
    retval = 0;
 done:
    return retval;
}

/*! Incoming commit handler for confirmed commit
 * @param[in]  h     Clicon handle
 * @param[in]  xe    Request: <rpc><xn></rpc> 
 * @param[in]  myid  Client-id
 * @param[out] cbret Return xml tree
 * @retval     1     OK
 * @retval     0     OK, dont proceed with commit
 * @retval    -1     Error
 */
int
from_client_confirmed_commit(clicon_handle h,
                             cxobj        *xe,
                             uint32_t      myid,
                             cbuf         *cbret)
{
    int retval = -1;
    int cc_valid;

    if ((cc_valid = check_valid_confirming_commit(h, xe, myid)) < 0)
        goto done;
    /* If <confirmed/> is *not* present, this will conclude the confirmed-commit, so cancel the rollback. */
    if (!xe_confirmed(xe) && cc_valid) {
        cancel_confirmed_commit(h);
        cprintf(cbret, "<rpc-reply xmlns=\"%s\"><ok/></rpc-reply>", NETCONF_BASE_NAMESPACE);
        goto dontcommit;
    }
    retval = 1;
 done:
    return retval;
 dontcommit:
    retval = 0;
    goto done;
}
