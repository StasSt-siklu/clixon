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

 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/time.h>
#include <regex.h>
#include <syslog.h>
#include <netinet/in.h>
#include <limits.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "restconf_lib.h"
#include "restconf_handle.h"

/* header part is copied from struct clicon_handle in lib/src/clixon_handle.c */

#define CLICON_MAGIC 0x99aafabe

#define handle(h) (assert(clicon_handle_check(h)==0),(struct restconf_handle *)(h))

/* Clicon_handle for backends.
 * First part of this is header, same for clicon_handle and cli_handle.
 * Access functions for common fields are found in clicon lib: clicon_options.[ch]
 * This file should only contain access functions for the _specific_
 * entries in the struct below.
 */
/*! Backend specific handle added to header CLICON handle
 * This file should only contain access functions for the _specific_
 * entries in the struct below.
 * @note The top part must be equivalent to struct clicon_handle in clixon_handle.c
 * @see struct clicon_handle, struct cli_handle
 */
struct restconf_handle {
    int                      rh_magic;     /* magic (HDR)*/
    clicon_hash_t           *rh_copt;      /* clicon option list (HDR) */
    clicon_hash_t           *rh_data;      /* internal clicon data (HDR) */
    clicon_hash_t           *rh_db_elmnt;  /* xml datastore element cache data */
    event_stream_t          *rh_stream;    /* notification streams, see clixon_stream.[ch] */
    
    /* ------ end of common handle ------ */
    clicon_hash_t           *rh_params;    /* restconf parameters, including http headers */
    clixon_auth_type_t       rh_auth_type; /* authentication type */
    int                      rh_pretty;    /* pretty-print for http replies */
    char                    *rh_fcgi_socket; /* if-feature fcgi, XXX: use WITH_RESTCONF_FCGI ? */
};

/*! Creates and returns a clicon config handle for other CLICON API calls
 */
clicon_handle
restconf_handle_init(void)
{
    struct restconf_handle *rh;

    rh = clicon_handle_init0(sizeof(struct restconf_handle));
    rh->rh_pretty = 1; /* clixon-restconf.yang : pretty is default true*/
    return rh;
}

/*! Deallocates a backend handle, including all client structs
 * @note: handle 'h' cannot be used in calls after this
 * @see backend_client_rm
 */
int
restconf_handle_exit(clicon_handle h)
{
    struct restconf_handle *rh = handle(h);
    
    if (rh->rh_fcgi_socket)
	free(rh->rh_fcgi_socket);
    clicon_handle_exit(h); /* frees h and options (and streams) */
    return 0;
}

/*! Get restconf http parameter
 * @param[in]  h     Clicon handle
 * @param[in]  name  Data name
 * @retval     val   Data value as string
 * Currently using clixon runtime data but there is risk for colliding names
 */
char *
restconf_param_get(clicon_handle h,
		   const char   *param)
{
    struct restconf_handle *rh = handle(h);

    if (rh->rh_params == NULL)
	return NULL;
    return (char*)clicon_hash_value(rh->rh_params, param, NULL);
}

/*! Set restconf http parameter
 * @param[in]  h    Clicon handle
 * @param[in]  name Data name
 * @param[in]  val  Data value as null-terminated string
 * @retval     0    OK
 * @retval    -1    Error
 * Currently using clixon runtime data but there is risk for colliding names
 */
int
restconf_param_set(clicon_handle h,
		   const char   *param,
		   char         *val)
{
    struct restconf_handle *rh = handle(h);

    clicon_debug(1, "%s: %s=%s", __FUNCTION__, param, val);
    if (rh->rh_params == NULL)
	if ((rh->rh_params = clicon_hash_init()) == NULL)
	    return -1;
    return clicon_hash_add(rh->rh_params, param, val, strlen(val)+1)==NULL?-1:0;
}

/*! Delete all restconf http parameter
 * @param[in]  h    Clicon handle
 * @param[in]  name Data name
 * @retval     0    OK
 * @retval    -1    Error
 * Currently using clixon runtime data but there is risk for colliding names
 */
int
restconf_param_del_all(clicon_handle h)
{
    int                     retval = -1;
    struct restconf_handle *rh = handle(h);
    
    if (rh->rh_params != NULL){
	if (clicon_hash_free(rh->rh_params) < 0)
	    goto done;
	rh->rh_params = NULL;
    }
    retval = 0;
 done:
    return retval;
}

/*! Get restconf http parameter
 * @param[in]  h         Clicon handle
 * @retval     auth_type
 */
clixon_auth_type_t
restconf_auth_type_get(clicon_handle h)
{
    struct restconf_handle *rh = handle(h);

    return rh->rh_auth_type;
}

/*! Set restconf http parameter
 * @param[in]  h    Clicon handle
 * @param[in]  name Data name
 * @param[in]  val  Data value as null-terminated string
 * @retval     0    OK
 * @retval    -1    Error
 * Currently using clixon runtime data but there is risk for colliding names
 */
int
restconf_auth_type_set(clicon_handle        h,
		       clixon_auth_type_t type)
{
    struct restconf_handle *rh = handle(h);

    rh->rh_auth_type = type;
    return 0;
}

/*! Get restconf pretty-print (for replies)
 * @param[in]  h         Clicon handle
 * @retval     pretty
 */
int
restconf_pretty_get(clicon_handle h)
{
    struct restconf_handle *rh = handle(h);

    return rh->rh_pretty;
}

/*! Set restconf pretty-print
 * @param[in]  h    Clicon handle
 * @param[in]  name Data name
 * @param[in]  val  Data value as null-terminated string
 * @retval     0    OK
 * @retval    -1    Error
 * Currently using clixon runtime data but there is risk for colliding names
 */
int
restconf_pretty_set(clicon_handle h,
		    int           pretty)
{
    struct restconf_handle *rh = handle(h);

    rh->rh_pretty = pretty;
    return 0;
}

/*! Get restconf fcgi socket path
 * @param[in]  h         Clicon handle
 * @retval     socketpath
 */
char*
restconf_fcgi_socket_get(clicon_handle h)
{
    struct restconf_handle *rh = handle(h);

    return rh->rh_fcgi_socket;
}

/*! Set restconf fcgi socketpath
 * @param[in]  h    Clicon handle
 * @param[in]  name Data name
 * @param[in]  val  Data value as null-terminated string
 * @retval     0    OK
 * @retval    -1    Error
 * Currently using clixon runtime data but there is risk for colliding names
 */
int
restconf_fcgi_socket_set(clicon_handle h,
			 char         *socketpath)
{
    struct restconf_handle *rh = handle(h);

    if ((rh->rh_fcgi_socket = strdup(socketpath)) == NULL){
	clicon_err(OE_RESTCONF, errno, "strdup");
	return -1;
    }
    return 0;
}
