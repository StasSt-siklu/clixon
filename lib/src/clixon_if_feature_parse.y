/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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

 * if-feature-expr RFC7950 14
   if-feature-expr     = if-feature-term
                           [sep or-keyword sep if-feature-expr]

   if-feature-term     = if-feature-factor
                           [sep and-keyword sep if-feature-term]

   if-feature-factor   = not-keyword sep if-feature-factor /
                         "(" optsep if-feature-expr optsep ")" /
                         identifier-ref-arg
  Called twice:
  1. In parsing just for syntac checks (dont do feature-check)
  2. In populate when all yangs are resolved, then do semantic feature-check
 */
%union {
    char *string;
    void *stack;
    int number;
}

%token MY_EOF
%token AND
%token NOT
%token OR
%token <string> TOKEN
%token SEP

%type <number> iffactor ifexpr

%start top

%lex-param     {void *_if} /* Add this argument to parse() and lex() function */
%parse-param   {void *_if}

%{

/* typecast macro */
#define _IF ((clixon_if_feature_yacc *)_if)

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <stdlib.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_string.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_yang_module.h"
#include "clixon_xml_vec.h"
#include "clixon_data.h"
#include "clixon_if_feature_parse.h"

/* Enable for debugging, steals some cycles otherwise */
#if 0
#define _PARSE_DEBUG(s) clicon_debug(1,(s))
#else
#define _PARSE_DEBUG(s)
#endif
    
void 
clixon_if_feature_parseerror(void *arg,
			     char *s) 
{
    clixon_if_feature_yacc *ife = (clixon_if_feature_yacc *)arg;

    clicon_err(OE_XML, XMLPARSE_ERRNO, "if_feature_parse: line %d: %s: at or before: %s", 
	       ife->if_linenum,
	       s,
	       clixon_if_feature_parsetext); 
    return;
}

/*! Check if feature "str" is enabled or not in context of yang node ys
 * @param[in]  str  feature str. 
 * @param[in]  ys   If-feature type yang node
 */
static int
if_feature_check(char      *str,
		 yang_stmt *ys)
{
    int         retval = -1;
    char       *prefix = NULL;
    char       *feature = NULL;
    yang_stmt  *ymod;  /* module yang node */
    yang_stmt  *yfeat; /* feature yang node */
    cg_var     *cv;
    
    if (ys == NULL)
	return 0;
    if (nodeid_split(str, &prefix, &feature) < 0)
	goto done;
    /* Specifically need to handle? strcmp(prefix, myprefix)) */
    if (prefix == NULL)
	ymod = ys_module(ys);
    else
	ymod = yang_find_module_by_prefix(ys, prefix);
    if (ymod == NULL)
	goto done;
    /* Check if feature exists, and is set, otherwise remove */
    if ((yfeat = yang_find(ymod, Y_FEATURE, feature)) == NULL){
	clicon_err(OE_YANG, EINVAL, "Yang module %s has IF_FEATURE %s, but no such FEATURE statement exists",
		   ymod?yang_argument_get(ymod):"none",
		   feature);
	goto done;
    }
    /* Check if this feature is enabled or not 
     * Continue loop to catch unbound features and make verdict at end
     */
    cv = yang_cv_get(yfeat);
    if (cv == NULL || !cv_bool_get(cv))    /* disabled */
	retval = 0;
    else                                  /* enabled */
	retval = 1;
 done:
    if (prefix)
	free(prefix);
    if (feature)
	free(feature);
    return retval;
}
 
%} 
 
%%

top        : ifexpr MY_EOF
                    {
			_PARSE_DEBUG("top->expr");
			_IF->if_enabled = $1;
			YYACCEPT;
		    }
           ;

ifexpr     : iffactor sep1 OR sep1 ifexpr
                    {
			_PARSE_DEBUG("expr->factor sep OR sep expr");
			$$ = $1 || $5;
		    }
           | iffactor sep1 AND sep1 ifexpr
	            {
			_PARSE_DEBUG("expr->factor sep AND sep expr");
			$$ = $1 && $5;
		    }
           | iffactor
	            {
			_PARSE_DEBUG("expr->factor");
			$$ = $1;
		    }
           ;

iffactor   : NOT sep1 iffactor     { _PARSE_DEBUG("factor-> NOT sep factor");
		                     $$ = !$3; }
           | '(' optsep ifexpr optsep ')'
		                   { _PARSE_DEBUG("factor-> ( optsep expr optsep )");
		                     $$ = $3; }
           | TOKEN                 {
	       _PARSE_DEBUG("factor->TOKEN");
	       if (_IF->if_ys == NULL) $$ = 0;
	       else if (($$ = if_feature_check($1, _IF->if_ys)) < 0) {
		   free($1);
		   YYERROR;
	       }
	       free($1);
	     }
           ;

optsep     : sep1                  { _PARSE_DEBUG("optsep->sep"); }
           |                       { _PARSE_DEBUG("optsep->"); }
           ;

sep1       : sep1 SEP              { _PARSE_DEBUG("sep->sep SEP"); }
           | SEP                   { _PARSE_DEBUG("sep->SEP"); }
           ;

%%
