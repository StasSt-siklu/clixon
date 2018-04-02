/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2018 Olof Hagsand and Benny Holmgren

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
#include <syslog.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_backend_handle.h"
#include "backend_commit.h"
#include "backend_plugin.h"
#include "backend_client.h"
#include "backend_handle.h"

/*! Add client notification subscription. Ie send notify to this client when event occurs
 * @param[in] ce      Client entry struct
 * @param[in] stream  Notification stream name
 * @param[in] format  How to display event (see enum format_enum)
 * @param[in] filter  Filter, what to display, eg xpath for format=xml, fnmatch
 *
 * @see backend_notify - where subscription is made and notify call is made
 */
static struct client_subscription *
client_subscription_add(struct client_entry *ce, 
			char                *stream, 
			enum format_enum     format,
			char                *filter)
{
    struct client_subscription *su = NULL;

    if ((su = malloc(sizeof(*su))) == NULL){
	clicon_err(OE_PLUGIN, errno, "malloc");
	goto done;
    }
    memset(su, 0, sizeof(*su));
    su->su_stream = strdup(stream);
    su->su_format = format;
    su->su_filter = filter?strdup(filter):strdup("");
    su->su_next   = ce->ce_subscription;
    ce->ce_subscription = su;
  done:
    return su;
}

static struct client_entry *
ce_find_bypid(struct client_entry *ce_list, int pid)
{
    struct client_entry *ce;

    for (ce = ce_list; ce; ce = ce->ce_next)
	if (ce->ce_pid == pid)
	    return ce;
    return NULL;
}

static int
client_subscription_delete(struct client_entry *ce, 
		    struct client_subscription *su0)
{
    struct client_subscription   *su;
    struct client_subscription  **su_prev;

    su_prev = &ce->ce_subscription; /* this points to stack and is not real backpointer */
    for (su = *su_prev; su; su = su->su_next){
	if (su == su0){
	    *su_prev = su->su_next;
	    free(su->su_stream);
	    if (su->su_filter)
		free(su->su_filter);
	    free(su);
	    break;
	}
	su_prev = &su->su_next;
    }
    return 0;
}

#ifdef notused /* xxx */
static struct client_subscription *
client_subscription_find(struct client_entry *ce, char *stream)
{
    struct client_subscription   *su = NULL;

    for (su = ce->ce_subscription; su; su = su->su_next)
	if (strcmp(su->su_stream, stream) == 0)
	    break;

    return su;
}
#endif 

/*! Remove client entry state
 * Close down everything wrt clients (eg sockets, subscriptions)
 * Finally actually remove client struct in handle
 * @param[in]  h   Clicon handle
 * @param[in]  ce  Client hadnle
 * @see backend_client_delete for actual deallocation of client entry struct
 */
int
backend_client_rm(clicon_handle        h, 
		  struct client_entry *ce)
{
    struct client_entry   *c;
    struct client_entry   *c0;
    struct client_entry  **ce_prev;
    struct client_subscription *su;

    c0 = backend_client_list(h);
    ce_prev = &c0; /* this points to stack and is not real backpointer */
    for (c = *ce_prev; c; c = c->ce_next){
	if (c == ce){
	    if (ce->ce_s){
		event_unreg_fd(ce->ce_s, from_client);
		close(ce->ce_s);
		ce->ce_s = 0;
	    }
	    while ((su = ce->ce_subscription) != NULL)
		client_subscription_delete(ce, su);
	    break;
	}
	ce_prev = &c->ce_next;
    }
    return backend_client_delete(h, ce); /* actually purge it */
}

/*! Find target/source in netconf request. Assume sanity- not finding is error */
static char*
netconf_db_find(cxobj *xn, 
		char  *name)
{
    cxobj *xs; /* source */
    cxobj *xi;
    char  *db = NULL;

    if ((xs = xml_find(xn, name)) == NULL)
	goto done;
    if ((xi = xml_child_i(xs, 0)) == NULL)
	goto done;
    db = xml_name(xi);
 done:
    return db;
}

/*! Internal message: get-config
 * 
 * @param[in]  h     Clicon handle
 * @param[in]  xe    Netconf request xml tree   
 * @param[out] cbret Return xml value cligen buffer
 */
static int
from_client_get_config(clicon_handle h,
		       cxobj        *xe,
		       cbuf         *cbret)
{
    int    retval = -1;
    char  *db;
    cxobj *xfilter;
    char  *selector = "/";
    cxobj *xret = NULL;
    cbuf  *cbx = NULL; /* Assist cbuf */
    
    if ((db = netconf_db_find(xe, "source")) == NULL){
	clicon_err(OE_XML, 0, "db not found");
	goto done;
    }
    if (xmldb_validate_db(db) < 0){
	if ((cbx = cbuf_new()) == NULL){
	    clicon_err(OE_XML, errno, "cbuf_new");
	    goto done;
	}	
	cprintf(cbx, "No such database: %s", db);
	if (netconf_invalid_value(cbret, "protocol", cbuf_get(cbx))< 0)
	    goto done;
	goto ok;
    }
    if ((xfilter = xml_find(xe, "filter")) != NULL)
	if ((selector = xml_find_value(xfilter, "select"))==NULL)
	    selector="/";
    if (xmldb_get(h, db, selector, 1, &xret) < 0){
	if (netconf_operation_failed(cbret, "application", "read registry")< 0)
	    goto done;
	goto ok;
    }
    cprintf(cbret, "<rpc-reply>");
    if (xret==NULL)
	cprintf(cbret, "<data/>");
    else{
	if (xml_name_set(xret, "data") < 0)
	    goto done;
	if (clicon_xml2cbuf(cbret, xret, 0, 0) < 0)
	    goto done;
    }
    cprintf(cbret, "</rpc-reply>");
 ok:
    retval = 0;
 done:
    if (cbx)
	cbuf_free(cbx);
    if (xret)
	xml_free(xret);
    return retval;
}

/*! Internal message: get
 * 
 * @param[in]  h     Clicon handle
 * @param[in]  xe    Netconf request xml tree   
 * @param[out] cbret Return xml value cligen buffer
 * @see from_client_get_config
 */
static int
from_client_get(clicon_handle h,
		cxobj        *xe,
		cbuf         *cbret)
{
    int    retval = -1;
    cxobj *xfilter;
    char  *selector = "/";
    cxobj *xret = NULL;
    int    ret;
    cbuf  *cbx = NULL; /* Assist cbuf */
    
    if ((xfilter = xml_find(xe, "filter")) != NULL)
	if ((selector = xml_find_value(xfilter, "select"))==NULL)
	    selector="/";
    /* Get config */
    if (xmldb_get(h, "running", selector, 0, &xret) < 0){
	if (netconf_operation_failed(cbret, "application", "read registry")< 0)
	    goto done;
	goto ok;
    }
    /* Get state data from plugins as defined by plugin_statedata(), if any */
    assert(xret);
    clicon_err_reset();
    if ((ret = clixon_plugin_statedata(h, selector, xret)) < 0)
	goto done;
    if (ret == 0){ /* OK */
	cprintf(cbret, "<rpc-reply>");
	if (xret==NULL)
	    cprintf(cbret, "<data/>");
	else{
	    if (xml_name_set(xret, "data") < 0)
		goto done;
	    if (clicon_xml2cbuf(cbret, xret, 0, 0) < 0)
		goto done;
	}
	cprintf(cbret, "</rpc-reply>");
    }
    else { /* 1 Error from callback */
	if ((cbx = cbuf_new()) == NULL){
	    clicon_err(OE_XML, errno, "cbuf_new");
	    goto done;
	}	
	cprintf(cbx, "Internal error:%s", clicon_err_reason);
	if (netconf_operation_failed(cbret, "rpc", cbuf_get(cbx))< 0)
	    goto done;
	clicon_log(LOG_NOTICE, "%s Error in backend_statedata_call:%s", __FUNCTION__, xml_name(xe));
    }
 ok:
    retval = 0;
 done:
    if (cbx)
	cbuf_free(cbx);
    if (xret)
	xml_free(xret);
    return retval;
}

/*! Internal message: edit-config
 * 
 * @param[in]  h      Clicon handle
 * @param[in]  xe     Netconf request xml tree   
 * @param[in]  mypid  Process/session id of calling client
 * @param[out] cbret  Return xml value cligen buffer
 */
static int
from_client_edit_config(clicon_handle h,
			cxobj        *xn,
			int           mypid,
			cbuf         *cbret)
{
    int                 retval = -1;
    char               *target;
    cxobj              *xc;
    cxobj              *x;
    enum operation_type operation = OP_MERGE;
    int                 piddb;
    int                 non_config = 0;
    yang_spec          *yspec;
    cbuf               *cbx = NULL; /* Assist cbuf */

    if ((yspec =  clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_YANG, ENOENT, "No yang spec");
	goto done;
    }
    if ((target = netconf_db_find(xn, "target")) == NULL){
	clicon_err(OE_XML, 0, "db not found");
	goto done;
    }
    if ((cbx = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }	
    if (xmldb_validate_db(target) < 0){
	cprintf(cbx, "No such database: %s", target);
	if (netconf_invalid_value(cbret, "protocol", cbuf_get(cbx))< 0)
	    goto done;
	goto ok;
    }
    /* Check if target locked by other client */
    piddb = xmldb_islocked(h, target);
    if (piddb && mypid != piddb){
	cprintf(cbx, "<session-id>%d</session-id>", piddb);
	if (netconf_lock_denied(cbret, cbuf_get(cbx), "Operation failed, lock is already held") < 0)
	    goto done;
	goto ok;
    }
    if ((x = xpath_first(xn, "default-operation")) != NULL){
	if (xml_operation(xml_body(x), &operation) < 0){
	    if (netconf_invalid_value(cbret, "protocol", "Wrong operation")< 0)
		goto done;
	    goto ok;
	}
    }
    if ((xc  = xpath_first(xn, "config")) == NULL){
	if (netconf_missing_element(cbret, "protocol", "<bad-element>config</bad-element>", NULL) < 0)
	    goto done;
	goto ok;
    }
    else{
	if (xml_apply(xc, CX_ELMNT, xml_spec_populate, yspec) < 0)
	    goto done;
	if (xml_apply(xc, CX_ELMNT, xml_non_config_data, &non_config) < 0)
	    goto done;
	if (non_config){
	    if (netconf_invalid_value(cbret, "protocol", "State data not allowed")< 0)
		goto done;
	    goto ok;
	}
	/* Cant do this earlier since we dont have a yang spec to
	 * the upper part of the tree, until we get the "config" tree.
	 */
	if (xml_child_sort && xml_apply0(xc, CX_ELMNT, xml_sort, NULL) < 0)
	    goto done;
	if (xmldb_put(h, target, operation, xc, cbret) < 0){
	    clicon_debug(1, "%s ERROR PUT", __FUNCTION__);	
	    if (netconf_operation_failed(cbret, "protocol", clicon_err_reason)< 0)
		goto done;
	    goto ok;
	}
    }
 ok:
    if (!cbuf_len(cbret))
	cprintf(cbret, "<rpc-reply><ok/></rpc-reply>");
    retval = 0;
 done:
    if (cbx)
	cbuf_free(cbx);
    clicon_debug(1, "%s done cbret:%s", __FUNCTION__, cbuf_get(cbret));	
    return retval;
} /* from_client_edit_config */

/*! Internal message: Lock database
 * 
 * @param[in]  h    Clicon handle
 * @param[in]  xe   Netconf request xml tree   
 * @param[in]  pid  Unix process id
 * @param[out] cbret Return xml value cligen buffer
 */
static int
from_client_lock(clicon_handle h,
		 cxobj        *xe,
		 int           pid,
		 cbuf         *cbret)
{
    int    retval = -1;
    char  *db;
    int    piddb;
    cbuf  *cbx = NULL; /* Assist cbuf */
    
    if ((db = netconf_db_find(xe, "target")) == NULL){
	if (netconf_missing_element(cbret, "protocol", "<bad-element>target</bad-element>", NULL) < 0)
	    goto done;
	goto ok;
    }
    if ((cbx = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }	
    if (xmldb_validate_db(db) < 0){
	cprintf(cbx, "No such database: %s", db);
	if (netconf_invalid_value(cbret, "protocol", cbuf_get(cbx))< 0)
	    goto done;
	goto ok;
    }
    /*
     * A lock MUST not be granted if either of the following conditions is true:
     * 1) A lock is already held by any NETCONF session or another entity.
     * 2) The target configuration is <candidate>, it has already been modified, and 
     *    these changes have not been committed or rolled back.
     */
    piddb = xmldb_islocked(h, db);
    if (piddb){
	cprintf(cbx, "<session-id>%d</session-id>", piddb);
	if (netconf_lock_denied(cbret, cbuf_get(cbx), "Operation failed, lock is already held") < 0)
	    goto done;
	goto ok;
    }
    else{
	if (xmldb_lock(h, db, pid) < 0)
	    goto done;
	cprintf(cbret, "<rpc-reply><ok/></rpc-reply>");
    }
 ok:
    retval = 0;
 done:
    if (cbx)
	cbuf_free(cbx);
    return retval;
}

/*! Internal message: Unlock database
 * 
 * @param[in]  h    Clicon handle
 * @param[in]  xe   Netconf request xml tree   
 * @param[in]  pid  Unix process id
 * @param[out] cbret Return xml value cligen buffer
 */
static int
from_client_unlock(clicon_handle h,
		   cxobj        *xe,
		   int           pid,
		   cbuf         *cbret)
{
    int    retval = -1;
    char  *db;
    int    piddb;
    cbuf  *cbx = NULL; /* Assist cbuf */

    if ((db = netconf_db_find(xe, "target")) == NULL){
	if (netconf_missing_element(cbret, "protocol", "<bad-element>target</bad-element>", NULL) < 0)
	    goto done;
	goto ok;
    }
    if ((cbx = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }	
    if (xmldb_validate_db(db) < 0){
	cprintf(cbx, "No such database: %s", db);
	if (netconf_invalid_value(cbret, "protocol", cbuf_get(cbx))< 0)
	    goto done;
	goto ok;
    }
    piddb = xmldb_islocked(h, db);
    /* 
     * An unlock operation will not succeed if any of the following
     * conditions are true:
     * 1) the specified lock is not currently active
     * 2) the session issuing the <unlock> operation is not the same
     *    session that obtained the lock
     */
    if (piddb==0 || piddb != pid){
	cprintf(cbx, "<session-id>pid=%d piddb=%d</session-id>", pid, piddb);
	if (netconf_lock_denied(cbret, cbuf_get(cbx), "Unlock failed, lock is already held") < 0)
	    goto done;
	goto ok;
    }
    else{
	xmldb_unlock(h, db);
	if (cprintf(cbret, "<rpc-reply><ok/></rpc-reply>") < 0)
	    goto done;
    }
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Internal message:  Kill session (Kill the process)
 * @param[in]  h     Clicon handle
 * @param[in]  xe    Netconf request xml tree   
 * @param[out] cbret Return xml value cligen buffer
 * @retval     0     OK
 * @retval    -1    Error. Send error message back to client.
 */
static int
from_client_kill_session(clicon_handle h,
			 cxobj        *xe,
			 cbuf         *cbret)
{
    int                  retval = -1;
    uint32_t             pid; /* other pid */
    char                *str;
    struct client_entry *ce;
    char                *db = "running"; /* XXX */
    cxobj               *x;
    
    if ((x = xml_find(xe, "session-id")) == NULL ||
	(str = xml_find_value(x, "body")) == NULL){
	if (netconf_missing_element(cbret, "protocol", "<bad-element>session-id</bad-element>", NULL) < 0)
	    goto done;
	goto ok;
    }
    pid = atoi(str);
    /* may or may not be in active client list, probably not */
    if ((ce = ce_find_bypid(backend_client_list(h), pid)) != NULL){
	xmldb_unlock_all(h, pid);	    
	backend_client_rm(h, ce);
    }
    
    if (kill (pid, 0) != 0 && errno == ESRCH) /* Nothing there */
	;
    else{
	killpg(pid, SIGTERM);
	kill(pid, SIGTERM);
#if 0 /* Hate sleeps we assume it died, see also 0 in next if.. */
	sleep(1);
#endif
    }
    if (1 || (kill (pid, 0) != 0 && errno == ESRCH)){ /* Nothing there */
	/* clear from locks */
	if (xmldb_islocked(h, db) == pid)
	    xmldb_unlock(h, db);
    }
    else{ /* failed to kill client */
	    if (netconf_operation_failed(cbret, "application", "Failed to kill session")< 0)
		goto done;
	goto ok;
    }
    cprintf(cbret, "<rpc-reply><ok/></rpc-reply>");
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Internal message: Copy database from db1 to db2
 * @param[in]   h      Clicon handle
 * @param[in]   xe     Netconf request xml tree   
 * @param[in]   mypid  Process/session id of calling client
 * @param[out]  cbret  Return xml value cligen buffer
 * @retval      0      OK
 * @retval      -1     Error. Send error message back to client.
 */
static int
from_client_copy_config(clicon_handle h,
			cxobj        *xe,
			int           mypid,
			cbuf         *cbret)
{
    char  *source;
    char  *target;
    int    retval = -1;
    int    piddb;
    cbuf  *cbx = NULL; /* Assist cbuf */
    
    if ((source = netconf_db_find(xe, "source")) == NULL){
	if (netconf_missing_element(cbret, "protocol", "<bad-element>source</bad-element>", NULL) < 0)
	    goto done;
	goto ok;
    }
    if ((cbx = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }	
    if (xmldb_validate_db(source) < 0){
	cprintf(cbx, "No such database: %s", source);
	if (netconf_invalid_value(cbret, "protocol", cbuf_get(cbx))< 0)
	    goto done;
	goto ok;
    }
    if ((target = netconf_db_find(xe, "target")) == NULL){
	if (netconf_missing_element(cbret, "protocol", "<bad-element>target</bad-element>", NULL) < 0)
	    goto done;
	goto ok;
    }
    if (xmldb_validate_db(target) < 0){
	cprintf(cbx, "No such database: %s", target);
	if (netconf_invalid_value(cbret, "protocol", cbuf_get(cbx))< 0)
	    goto done;
	goto ok;
    }
    /* Check if target locked by other client */
    piddb = xmldb_islocked(h, target);
    if (piddb && mypid != piddb){
	cprintf(cbx, "<session-id>%d</session-id>", piddb);
	if (netconf_lock_denied(cbret, cbuf_get(cbx), "Copy failed, lock is already held") < 0)
	    goto done;
	goto ok;
    }
    if (xmldb_copy(h, source, target) < 0){
	if (netconf_operation_failed(cbret, "application", clicon_err_reason)< 0)
	    goto done;
	goto ok;
    }
    cprintf(cbret, "<rpc-reply><ok/></rpc-reply>");
 ok:
    retval = 0;
 done:
    if (cbx)
	cbuf_free(cbx);
    return retval;
}

/*! Internal message: Delete database
 * @param[in]   h     Clicon handle
 * @param[in]   xe    Netconf request xml tree   
 * @param[in]   mypid Process/session id of calling client
 * @param[out]  cbret Return xml value cligen buffer
 * @retval      0     OK
 * @retval      -1    Error. Send error message back to client.
 */
static int
from_client_delete_config(clicon_handle h,
			  cxobj        *xe,
			  int           mypid,
			  cbuf         *cbret)
{
    int    retval = -1;
    char  *target;
    int    piddb;
    cbuf  *cbx = NULL; /* Assist cbuf */

    if ((target = netconf_db_find(xe, "target")) == NULL||
	strcmp(target, "running")==0){
	if (netconf_missing_element(cbret, "protocol", "<bad-element>target</bad-element>", NULL) < 0)
	    goto done;
	goto ok;
    }
    if ((cbx = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }	
    if (xmldb_validate_db(target) < 0){
	cprintf(cbx, "No such database: %s", target);
	if (netconf_invalid_value(cbret, "protocol", cbuf_get(cbx))< 0)
	    goto done;
	goto ok;
    }
    /* Check if target locked by other client */
    piddb = xmldb_islocked(h, target);
    if (piddb && mypid != piddb){
	cprintf(cbx, "<session-id>%d</session-id>", piddb);
	if (netconf_lock_denied(cbret, cbuf_get(cbx), "Operation failed, lock is already held") < 0)
	    goto done;
	goto ok;
    }
    if (xmldb_delete(h, target) < 0){
	if (netconf_operation_failed(cbret, "protocol", clicon_err_reason)< 0)
	    goto done;
	goto ok;
    }
    if (xmldb_create(h, target) < 0){
	if (netconf_operation_failed(cbret, "protocol", clicon_err_reason)< 0)
	    goto done;
	goto ok;
    }
    cprintf(cbret, "<rpc-reply><ok/></rpc-reply>");
 ok:
    retval = 0;
  done:
    if (cbx)
	cbuf_free(cbx);
    return retval;
}

/*! Internal message: Create subscription for notifications see RFC 5277
 * @param[in]   h     Clicon handle
 * @param[in]   xe    Netconf request xml tree   
 * @param[in]   ce    Client entry
 * @param[out]  cbret Return xml value cligen buffer
 * @retval      0    OK
 * @retval      -1   Error. Send error message back to client.
 * @example:
 *    <create-subscription> 
 *       <stream>RESULT</stream> # If not present, events in the default NETCONF stream will be sent.
 *       <filter>XPATH-EXPR<(filter>
 *       <startTime/> # only for replay (NYI)
 *       <stopTime/>  # only for replay (NYI)
 *    </create-subscription> 
 */
static int
from_client_create_subscription(clicon_handle        h,
				cxobj               *xe,
				struct client_entry *ce,
				cbuf                *cbret)
{
    char   *stream = "NETCONF";
    char   *filter = NULL;
    int     retval = -1;
    cxobj  *x; /* Genereic xml tree */
    char   *ftype;

    if ((x = xpath_first(xe, "//stream")) != NULL)
	stream = xml_find_value(x, "body");
    if ((x = xpath_first(xe, "//filter")) != NULL){
	if ((ftype = xml_find_value(x, "type")) != NULL){
	    /* Only accept xpath as filter type */
	    if (strcmp(ftype, "xpath") != 0){
		if (netconf_operation_failed(cbret, "application", "Only xpath filter type supported")< 0)
		    goto done;
		goto ok;
	    }
	}
    }
    if (client_subscription_add(ce, stream, FORMAT_XML, filter) == NULL)
	goto done;
    cprintf(cbret, "<rpc-reply><ok/></rpc-reply>");
 ok:
    retval = 0;
  done:
    return retval;
}

/*! Internal message: Set debug level. This is global, not just for the session.
 * @param[in]   h     Clicon handle
 * @param[in]   xe    Netconf request xml tree   
 * @param[out]  cbret Return xml value cligen buffer
 * @retval      0     OK
 * @retval      -1    Error. Send error message back to client.
 */
static int
from_client_debug(clicon_handle      h,
		  cxobj             *xe,
		  cbuf              *cbret)
{
    int      retval = -1;
    uint32_t level;
    char    *valstr;
    
    if ((valstr = xml_find_body(xe, "level")) == NULL){
	if (netconf_missing_element(cbret, "application", "<bad-element>level</bad-element>", NULL) < 0)
	    goto done;
	goto ok;
    }
    level = atoi(valstr);

    clicon_debug_init(level, NULL); /* 0: dont debug, 1:debug */
    setlogmask(LOG_UPTO(level?LOG_DEBUG:LOG_INFO)); /* for syslog */
    clicon_log(LOG_NOTICE, "%s debug:%d", __FUNCTION__, debug);
    cprintf(cbret, "<rpc-reply><ok/></rpc-reply>");
 ok:
    retval = 0;
 done:
    return retval;
}

/*! An internal clicon message has arrived from a client. Receive and dispatch.
 * @param[in]   s    Socket where message arrived. read from this.
 * @param[in]   arg  Client entry (from).
 * @retval      0    OK
 * @retval      -1   Error Terminates backend and is never called). Instead errors are
 *                   propagated back to client.
 */
static int
from_client_msg(clicon_handle        h,
		struct client_entry *ce, 
		struct clicon_msg   *msg)
{
    int                  retval = -1;
    cxobj               *xt = NULL;
    cxobj               *x;
    cxobj               *xe;
    char                *name = NULL;
    char                *db;
    cbuf                *cbret = NULL; /* return message */
    int                  pid;
    int                  ret;

    pid = ce->ce_pid;
    /* Return netconf message. Should be filled in by the dispatch(sub) functions 
     * as wither rpc-error or by positive response.
     */
    if ((cbret = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    if (clicon_msg_decode(msg, &xt) < 0){
	if (netconf_malformed_message(cbret, "Not recognized, rpc expected")< 0)
	    goto done;
	goto reply;
    }
    if ((x = xpath_first(xt, "/rpc")) == NULL){
	if (netconf_malformed_message(cbret, "Not recognized, rpc expected")< 0)
	    goto done;
	goto reply;
    }
    xe = NULL;
    while ((xe = xml_child_each(x, xe, CX_ELMNT)) != NULL) {
	name = xml_name(xe);
	if (strcmp(name, "get-config") == 0){
	    if (from_client_get_config(h, xe, cbret) <0)
		goto done;
	}
	else if (strcmp(name, "edit-config") == 0){
	    if (from_client_edit_config(h, xe, pid, cbret) <0)
		goto done;
	}
	else if (strcmp(name, "copy-config") == 0){
	    if (from_client_copy_config(h, xe, pid, cbret) <0)
		goto done;
	}
	else if (strcmp(name, "delete-config") == 0){
	    if (from_client_delete_config(h, xe, pid, cbret) <0)
		goto done;
	}
	else if (strcmp(name, "lock") == 0){
	    if (from_client_lock(h, xe, pid, cbret) < 0)
		goto done;
	}
	else if (strcmp(name, "unlock") == 0){
	    if (from_client_unlock(h, xe, pid, cbret) < 0)
		goto done;
	}
	else if (strcmp(name, "get") == 0){
	    if (from_client_get(h, xe, cbret) < 0)
		goto done;
	}
	else if (strcmp(name, "close-session") == 0){
	    xmldb_unlock_all(h, pid);
	    cprintf(cbret, "<rpc-reply><ok/></rpc-reply>");
	}
	else if (strcmp(name, "kill-session") == 0){
	    if (from_client_kill_session(h, xe, cbret) < 0)
		goto done;
	}
	else if (strcmp(name, "validate") == 0){
	    if ((db = netconf_db_find(xe, "source")) == NULL){
		if (netconf_missing_element(cbret, "protocol", "<bad-element>source</bad-element>", NULL) < 0)
		    goto done;
		goto reply;
	    }
	    if (from_client_validate(h, db, cbret) < 0)
		goto done;
	}
	else if (strcmp(name, "commit") == 0){
	    if (from_client_commit(h, pid, cbret) < 0)
		goto done;
	}
	else if (strcmp(name, "discard-changes") == 0){
	    if (from_client_discard_changes(h, pid, cbret) < 0)
		goto done;
	}
	else if (strcmp(name, "create-subscription") == 0){
	    if (from_client_create_subscription(h, xe, ce, cbret) < 0)
		goto done;
	}
	else if (strcmp(name, "debug") == 0){
	    if (from_client_debug(h, xe, cbret) < 0)
		goto done;
	}
	else{
	    clicon_err_reset();
	    if ((ret = backend_rpc_cb_call(h, xe, ce, cbret)) < 0){
		if (netconf_operation_failed(cbret, "application", clicon_err_reason)< 0)
		    goto done;
		clicon_log(LOG_NOTICE, "%s Error in backend_rpc_call:%s", __FUNCTION__, xml_name(xe));
		goto reply; /* Dont quit here on user callbacks */
	    }
	    if (ret == 0){ /* not handled by callback */
		if (netconf_operation_failed(cbret, "application", "Callback not recognized")< 0)
		    goto done;
		goto reply;
	    }
	}
    }
 reply:
    if (cbuf_len(cbret) == 0)
	if (netconf_operation_failed(cbret, "application", clicon_err_reason)< 0)
	    goto done;
    clicon_debug(1, "%s cbret:%s", __FUNCTION__, cbuf_get(cbret));
    if (send_msg_reply(ce->ce_s, cbuf_get(cbret), cbuf_len(cbret)+1) < 0){
	switch (errno){
	case EPIPE:
	    /* man (2) write: 
	     * EPIPE  fd is connected to a pipe or socket whose reading end is 
	     * closed.  When this happens the writing process will also receive 
	     * a SIGPIPE signal. 
	     * In Clixon this means a client, eg restconf, netconf or cli closes
	     * the (UNIX domain) socket.
	     */
	case ECONNRESET:
	    clicon_log(LOG_WARNING, "client rpc reset");
	    break;
	default:
	    goto done;
	}
    }
    // ok:
    retval = 0;
  done:  
    if (xt)
	xml_free(xt);
    if (cbret)
	cbuf_free(cbret);
    /* Sanity: log if clicon_err() is not called ! */
    if (retval < 0 && clicon_errno < 0) 
	clicon_log(LOG_NOTICE, "%s: Internal error: No clicon_err call on error (message: %s)",
		   __FUNCTION__, name?name:"");
    //    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    return retval;// -1 here terminates backend
}

/*! An internal clicon message has arrived from a client. Receive and dispatch.
 * @param[in]   s    Socket where message arrived. read from this.
 * @param[in]   arg  Client entry (from).
 * @retval      0    OK
 * @retval      -1   Error Terminates backend and is never called). Instead errors are
 *                   propagated back to client.
 */
int
from_client(int   s, 
	    void* arg)
{
    int                  retval = -1;
    struct clicon_msg   *msg = NULL;
    struct client_entry *ce = (struct client_entry *)arg;
    clicon_handle        h = ce->ce_handle;
    int                  eof;

    // assert(s == ce->ce_s);
    if (clicon_msg_rcv(ce->ce_s, &msg, &eof) < 0)
	goto done;
    if (eof)
	backend_client_rm(h, ce); 
    else
	if (from_client_msg(h, ce, msg) < 0)
	    goto done;
    retval = 0;
  done:
    clicon_debug(1, "%s retval=%d", __FUNCTION__, retval);
    if (msg)
	free(msg);
    return retval; /* -1 here terminates backend */
}
