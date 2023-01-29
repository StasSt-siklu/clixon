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

  * Utility to validate and/or commit as a single utility, to be used in eg shell scripts
  * Does much of what backend_main.c does, only less so
  * Example:
  * 1) validate foo_db using a tmp dbdir
  * ./clixon_util_validate -f /usr/local/etc/example.xml -d foo -o  CLICON_XMLDB_DIR=/tmp
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <syslog.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/stat.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon/clixon.h"

/* For validate and commit commands. */
#include "clixon/clixon_backend.h"

/* Command line options passed to getopt(3) */
#define UTIL_COMMIT_OPTS "hD:f:cd:o:"

static int
usage(char *argv0)
{
    fprintf(stderr, "Tool to validate a database\nusage:%s [options]\n"
            "where options are\n"
            "\t-h \t\tHelp\n"
            "\t-D <level> \tDebug\n"
            "\t-f <file>\tClixon config file\n"
            "\t-d <file>\tDatabase name (if not candidate, must be in XMLDBDIR)\n"
            "\t-c \t\tValidate + commit, otherwise only validate\n"
            "\t-o \"<option>=<value>\"\tGive configuration option overriding config file (see clixon-config.yang)\n",
            argv0);
    exit(0);
}

int
main(int    argc,
     char **argv)
{
    int           retval = -1;
    int           c;
    yang_stmt    *yspec = NULL;
    int           commit = 0;
    char         *database = NULL;
    clicon_handle h;
    int           dbg = 0;
    char         *dir;
    char         *str;
    int           ret;
    cbuf         *cb = NULL;

    /* In the startup, logs to stderr & debug flag set later */
    clicon_log_init(__FILE__, LOG_INFO, CLICON_LOG_STDERR); 

    /* Initialize clixon handle */
    if ((h = clicon_handle_init()) == NULL)
        goto done;
    /*
     * Command-line options for help, debug, and config-file
     */
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, UTIL_COMMIT_OPTS)) != -1)
        switch (c) {
        case 'h':
            usage(argv[0]);
            break;
        case 'D':
            if (sscanf(optarg, "%d", &dbg) != 1)
                usage(argv[0]);
            break;
        case 'f': /* config file */
            if (!strlen(optarg))
                usage(argv[0]);
            clicon_option_str_set(h, "CLICON_CONFIGFILE", optarg);
            break;
        case 'c': /* commit (otherwise only validate) */
        case 'd': /* candidate database (if not candidate) */
        case 'o': /* Configuration option */
            break; /* see next getopt */
        default:
            usage(argv[0]);
            break;
        }
    clicon_debug_init(dbg, NULL);
    yang_init(h);
    /* Find and read configfile */
    if (clicon_options_main(h) < 0)
        goto done;
    /* Initialize plugin module by creating a handle holding plugin and callback lists */
    if (clixon_plugin_module_init(h) < 0)
        goto done;
    /* Now run through the operational args */
    opterr = 1;
    optind = 1;
    while ((c = getopt(argc, argv, UTIL_COMMIT_OPTS)) != -1)
        switch (c) {
        case 'h' : /* help */
        case 'D' : /* debug */
        case 'f': /* config file */
            break;
        case 'c': /* commit (otherwise only validate) */
            commit++;
            break;
        case 'd': /* candidate database (if not candidate) */
            database = optarg;
            break;
        case 'o':{ /* Configuration option */
            char          *val;
            if ((val = index(optarg, '=')) == NULL)
                usage(argv[0]);
            *val++ = '\0';
            if (clicon_option_add(h, optarg, val) < 0)
                goto done;
            break;
        }
        default:
            usage(argv[0]);
            break;
        }

    /* Set default namespace according to CLICON_NAMESPACE_NETCONF_DEFAULT */
    xml_nsctx_namespace_netconf_default(h);
    
    /* Add (hardcoded) netconf features in case ietf-netconf loaded here
     * Otherwise it is loaded in netconf_module_load below
     */
    if (netconf_module_features(h) < 0)
        goto done;

    /* Create top-level yang spec and store as option */
    if ((yspec = yspec_new()) == NULL)
        goto done;
    clicon_dbspec_yang_set(h, yspec);       
    /* Load backend plugins before yangs are loaded (eg extension callbacks) */
    if ((dir = clicon_backend_dir(h)) != NULL &&
        clixon_plugins_load(h, CLIXON_PLUGIN_INIT, dir,
                            clicon_option_str(h, "CLICON_BACKEND_REGEXP")) < 0)
        goto done;
    /* Load Yang modules
     * 1. Load a yang module as a specific absolute filename */
    if ((str = clicon_yang_main_file(h)) != NULL)
        if (yang_spec_parse_file(h, str, yspec) < 0)
            goto done;
    /* 2. Load a (single) main module */
    if ((str = clicon_yang_module_main(h)) != NULL)
        if (yang_spec_parse_module(h, str, clicon_yang_module_revision(h),
                                   yspec) < 0)
            goto done;
    /* 3. Load all modules in a directory (will not overwrite file loaded ^) */
    if ((str = clicon_yang_main_dir(h)) != NULL)
        if (yang_spec_load_dir(h, str, yspec) < 0)
            goto done;
    /* Load clixon lib yang module */
    if (yang_spec_parse_module(h, "clixon-lib", NULL, yspec) < 0)
        goto done;
    /* Load yang module library, RFC7895 */
    if (yang_modules_init(h) < 0)
        goto done;
    /* Add generic yang specs, used by netconf client and as internal protocol 
     */
    if (netconf_module_load(h) < 0)
        goto done;
    /* Load yang restconf module */
    if (yang_spec_parse_module(h, "ietf-restconf", NULL, yspec)< 0)
        goto done;
    /* Load yang YANG module state */
    if (clicon_option_bool(h, "CLICON_XMLDB_MODSTATE") &&
        yang_spec_parse_module(h, "ietf-yang-library", NULL, yspec)< 0)
        goto done;
    /* Here all modules are loaded */
    if (database == NULL)
        database = "candidate";
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if (commit){
        if ((ret = candidate_commit(h, NULL, database, 0, VL_FULL, cb)) < 0)
            goto done;
    }
    else{
        if ((ret = candidate_validate(h, database, cb)) < 0)
            goto done;
    }
    if (ret == 0){
        clicon_err(OE_DB, 0, " Failed: %s", cbuf_get(cb));
        goto done;
    }
    fprintf(stdout, "OK\n");
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

