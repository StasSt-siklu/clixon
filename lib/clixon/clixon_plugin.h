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
/*
 * Internal prototypes, not accessed by plugin client code
 */

#ifndef _CLIXON_PLUGIN_H_
#define _CLIXON_PLUGIN_H_

/*
 * Types
 */
/* The dynamicically loadable plugin object handle */
typedef void *plghndl_t;

/*
 * Prototypes
 */
/* Common plugin function names, function types and signatures. 
 * This plugin code is exytended by backend, cli, netconf, restconf plugins
 *   Cli     see cli_plugin.c
 *   Backend see config_plugin.c
 */

/*! Called when plugin loaded. Only mandadory callback. All others optional 
 * @see plginit_t
 */
#define PLUGIN_INIT            "plugin_init"

typedef void * (plginit_t)(clicon_handle);    /* Clixon plugin Init */

/* Called when backend started with cmd-line arguments from daemon call. 
 * @see plgstart_t
 */
#define PLUGIN_START           "plugin_start"
typedef int (plgstart_t)(clicon_handle, int, char **); /* Plugin start */

/* Called just before plugin unloaded. 
 */
#define PLUGIN_EXIT            "plugin_exit"
typedef int (plgexit_t)(clicon_handle);		       /* Plugin exit */

/*! Called by restconf to check credentials and return username
 */

/* Plugin authorization. Set username option (or not)
 * @param[in]  Clicon handle
 * @param[in]  void*, eg Fastcgihandle request restconf
 * @retval   0 if credentials OK
 * @retval  -1 credentials not OK
 */
typedef int (plgauth_t)(clicon_handle, void *);

typedef int (plgreset_t)(clicon_handle h, const char *db); /* Reset system status */
typedef int (plgstatedata_t)(clicon_handle h, char *xpath, cxobj *xtop);

typedef void *transaction_data;

/* Transaction callbacks */
typedef int (trans_cb_t)(clicon_handle h, transaction_data td); 

/* plugin init struct for the api 
 * Note: Implicit init function
 */
struct clixon_plugin_api;
typedef struct clixon_plugin_api* (plginit2_t)(clicon_handle);    /* Clixon plugin Init */

struct clixon_plugin_api{
    char              ca_name[PATH_MAX]; /* Name of plugin (given by plugin) */
    plginit2_t       *ca_init;           /* Clixon plugin Init (implicit) */
    plgstart_t       *ca_start;          /* Plugin start */
    plgexit_t        *ca_exit;	         /* Plugin exit */
    plgauth_t        *ca_auth;           /* Auth credentials */
    /*--Above here common fields w clixon_backend_api  ----------*/
    plgreset_t       *ca_reset;          /* Reset system status (backend only) */

    plgstatedata_t   *ca_statedata;      /* Get state data from plugin (backend only) */
    trans_cb_t       *ca_trans_begin;	 /* Transaction start */
    trans_cb_t       *ca_trans_validate; /* Transaction validation */
    trans_cb_t       *ca_trans_complete; /* Transaction validation complete */
    trans_cb_t       *ca_trans_commit;   /* Transaction commit */
    trans_cb_t       *ca_trans_end;	 /* Transaction completed  */
    trans_cb_t       *ca_trans_abort;	 /* Transaction aborted */    
};
typedef struct clixon_plugin_api clixon_plugin_api;

/*! Called when plugin loaded. Only mandadory callback. All others optional 
 * @see plginit_t
 */

/* Internal plugin structure with dlopen() handle and plugin_api
 */
struct clixon_plugin{
    char                     cp_name[PATH_MAX]; /* Plugin filename. Note api ca_name is given by plugin itself */
    plghndl_t                cp_handle;  /* Handle to plugin using dlopen(3) */
    struct clixon_plugin_api cp_api;
};
typedef struct clixon_plugin clixon_plugin;

/* 
 * Pseudo-Prototypes 
 * User-defineed plugins, not in library code 
 */
#define CLIXON_PLUGIN_INIT     "clixon_plugin_init" /* Nextgen */

/*! Plugin initialization
 * @param[in]  h    Clixon handle
 * @retval     NULL Error with clicon_err set
 * @retval     api  Pointer to API struct
 */
clixon_plugin_api *clixon_plugin_init(clicon_handle h);

/*
 * Prototypes
 */
clixon_plugin *plugin_each(clixon_plugin *cpprev);
clixon_plugin *plugin_each_revert(clixon_plugin *cpprev, int nr);

int clixon_plugins_load(clicon_handle h, char *function, char *dir);

/* obsolete */
plghndl_t plugin_load (clicon_handle h, char *file, int dlflags);

/* obsolete */
int plugin_unload(clicon_handle h, plghndl_t *handle);

int clixon_plugin_start(clicon_handle h, int argc, char **argv);

int clixon_plugin_exit(clicon_handle h);

int clixon_plugin_auth(clicon_handle h, void *arg);

#endif  /* _CLIXON_PLUGIN_H_ */
