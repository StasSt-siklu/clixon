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

 * Clixon XML object vectors
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <assert.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_err.h"
#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_log.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_xml_io.h"
#include "clixon_xml_vec.h"

//typedef struct clixon_xml_vec clixon_xvec; /* struct defined in clicon_xml_vec.c */

/* How many XML children to start with if any (and then add exponentialy) */
#define XVEC_MAX_DEFAULT 4      /* start value */
#define XVEC_MAX_THRESHOLD 1024 /* exponential growth to here, then linear */

/*! Clixon xml vector concrete implementaion of the abstract clixon_xvec type
 * Contiguous vector (not linked list) so that binary search can be done by direct index access
 */
struct clixon_xml_vec {
    cxobj **xv_vec;   /* Sorted vector of xml object pointers */
    int     xv_len;   /* Length of vector */
    int     xv_max;   /* Vector allocation */    
};

/*! Increment cxobj vector in an XML object vector 
 *
 * Exponential growth to a threshold, then linear
 * @param[in]  xv    XML tree vector
 * @retval     0     OK
 * @retval    -1     Error
 */
static int
clixon_xvec_inc(clixon_xvec *xv)
{
    int retval = -1;
    
    xv->xv_len++;
    if (xv->xv_len > xv->xv_max){
	if (xv->xv_max < XVEC_MAX_DEFAULT)
	    xv->xv_max = XVEC_MAX_DEFAULT;
	else if (xv->xv_max < XVEC_MAX_THRESHOLD)
	    xv->xv_max *= 2;                  /* Double the space - exponential */
	else
	    xv->xv_max += XVEC_MAX_THRESHOLD; /* Add - linear growth */
	if ((xv->xv_vec = realloc(xv->xv_vec, sizeof(cxobj *) * xv->xv_max)) == NULL){
	    clicon_err(OE_XML, errno, "realloc");
	    goto done;
	}
    }
    retval = 0;
 done:
    return retval;
}

/*! Create new XML object vector
 *
 * Exponential growth to a threshold, then linear
 * @param[in]  xv    XML tree vector
 * @retval     0     OK
 * @retval    -1     Error
 */
clixon_xvec *
clixon_xvec_new(void)
{
    clixon_xvec *xv = NULL;

    if ((xv = malloc(sizeof(*xv))) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    memset(xv, 0, sizeof(*xv));
    xv->xv_len = 0;
    xv->xv_max = 0;

 done:
    return xv;
}

/*! Create and copy XML vector
 *
 * @param[in]  xv0    XML tree vector
 * @retval     xv1    Duplicated XML vector
 * @retval     NULL   Error
 */
clixon_xvec *
clixon_xvec_dup(clixon_xvec *xv0)
{
    clixon_xvec *xv1 = NULL; /* retval */

    if ((xv1 = clixon_xvec_new()) == NULL)
	goto done;
    *xv1 = *xv0;
    xv1->xv_vec = NULL;
    if (xv1->xv_max &&
	(xv1->xv_vec = calloc(xv1->xv_max, sizeof(cxobj*))) == NULL){
	clicon_err(OE_UNIX, errno, "calloc");
	free(xv1);
	xv1 = NULL;
	goto done;
    }
    memcpy(xv1->xv_vec, xv0->xv_vec, xv0->xv_len*sizeof(cxobj*));
 done:
    return xv1;
}

/*! Free XML object list
 */
int
clixon_xvec_free(clixon_xvec *xv)
{
    if (xv->xv_vec)
	free(xv->xv_vec);
    if (xv)
	free(xv);
    return 0;
}

/*! Return length of XML object list
 * @param[in]  xv    XML tree vector
 * @retval     len   Length of XML object vector
 */
int
clixon_xvec_len(clixon_xvec *xv)
{
    return xv->xv_len;
}

/*! Return i:th XML object in XML object vector
 * @param[in]  xv    XML tree vector
 * @retval     x     OK
 * @retval     NULL  Not found
 */
cxobj*
clixon_xvec_i(clixon_xvec *xv,
	      int          i)
{
    if (i < xv->xv_len)
	return xv->xv_vec[i];
    else
	return NULL;
}

/*! Return whole XML object vector and null it in original xvec, essentially moving it
 *
 * Used in glue code between clixon_xvec code and cxobj **, size_t code, may go AWAY?
 * @param[in]  xv    XML tree vector
 * @param[out] xvec  XML object vector
 * @param[out] xlen  Number of elements
 * @param[out] xmax  Length of allocated vector
 * @retval     0     OK
 * @retval    -1     Error
 */
int
clixon_xvec_extract(clixon_xvec *xv,
		    cxobj     ***xvec,
		    int         *xlen,
		    int         *xmax)
{
    int retval = -1;
    
    if (xv == NULL){
	clicon_err(OE_XML, EINVAL, "xv is NULL");
	goto done;
    }
    *xvec = xv->xv_vec;
    *xlen = xv->xv_len;
    if (xmax)
	*xmax = xv->xv_max;
    if (xv->xv_vec != NULL){
	xv->xv_len = 0;
	xv->xv_max = 0;
	xv->xv_vec = NULL;
    }
    retval = 0;
 done:
    return retval;
}

/*! Append a new xml tree to an existing xml vector last in the list
 *
 * @param[in]  xv    XML tree vector
 * @param[in]  x     XML tree (append this to vector)
 * @retval     0     OK
 * @retval    -1     Error
 * @code
 *  if (clixon_xvec_append(xv, x) < 0) 
 *     err;
 * @endcode
 * @see clixon_cxvec_prepend
 */
int
clixon_xvec_append(clixon_xvec *xv,
		   cxobj       *x)
		   
{
    int retval = -1;

    if (clixon_xvec_inc(xv) < 0)
	goto done;
    xv->xv_vec[xv->xv_len-1] = x;
    retval = 0;
 done:
    return retval;
}

/*! Append a second clixon-xvec into a first
 *
 * @param[in,out] xv0   XML tree vector
 * @param[in]     xv1   XML tree (append this to vector)
 * @retval        0     OK, with xv0 with new entries from xv1
 * @retval       -1     Error
 * @code
 *  if (clixon_xvec_merge(xv0, xv1) < 0) 
 *     err;
 * @endcode
 */
int
clixon_xvec_merge(clixon_xvec *xv0,
		  clixon_xvec *xv1)
{
    int    retval = -1;
    cxobj *x;
    int    i;

    for (i=0; i<clixon_xvec_len(xv1); i++){
	x = clixon_xvec_i(xv1, i);
	if (clixon_xvec_inc(xv0) < 0)
	    goto done;
	xv0->xv_vec[xv0->xv_len-1] = x;
    }
    retval = 0;
 done:
    return retval;
}

/*! Prepend a new xml tree to an existing xml vector first in the list
 *
 * @param[in]  xv    XML tree vector
 * @param[in]  x     XML tree (append this to vector)
 * @retval     0     OK
 * @retval    -1     Error
 * @code
 *  if (clixon_xvec_prepend(xv, x) < 0) 
 *     err;
 * @endcode
 * @see clixon_cxvec_append
 */
int
clixon_xvec_prepend(clixon_xvec *xv,
		    cxobj       *x)
{
    int retval = -1;

    if (clixon_xvec_inc(xv) < 0)
	goto done;
    memmove(&xv->xv_vec[1], &xv->xv_vec[0], sizeof(cxobj *) * (xv->xv_len-1));
    xv->xv_vec[0] = x;
    retval = 0;
 done:
    return retval;
}

/*! Insert XML node x at position i in XML object vector
 * 
 * @param[in]  xv    XML tree vector
 * @param[in]  x     XML tree (append this to vector)
 * @param[in]  i     Position
 * @retval     0     OK
 * @retval    -1     Error
 */
int
clixon_xvec_insert_pos(clixon_xvec *xv,
		       cxobj       *x,
		       int          i)
{
    int    retval = -1;
    size_t size;
    
    if (clixon_xvec_inc(xv) < 0)
	goto done;
    size = (xv->xv_len - i -1)*sizeof(cxobj *);
    memmove(&xv->xv_vec[i+1], &xv->xv_vec[i], size);
    xv->xv_vec[i] = x;
    retval = 0;
 done:
    return retval;
}

/*! Remove XML node x from position i in XML object vector
 * 
 * @param[in]  xv    XML tree vector
 * @param[in]  i     Position
 * @retval     0     OK
 * @retval    -1     Error
 */
int
clixon_xvec_rm_pos(clixon_xvec *xv,
		   int          i)
{
    size_t size;
    
    size = (xv->xv_len - i + 1)*sizeof(cxobj *);
    memmove(&xv->xv_vec[i], &xv->xv_vec[i+1], size);
    xv->xv_len--;
    return 0;
}

/*! Print an XML object vector to an output stream and encode chars "<>&"
 *
 * @param[in]  f     UNIX output stream
 * @param[in]  xv    XML tree vector
 * @retval     0     OK
 */
int
clixon_xvec_print(FILE        *f,
		  clixon_xvec *xv)
{
    int i;
    
    for (i=0; i<xv->xv_len; i++)
	clicon_xml2file(f, xv->xv_vec[i], 0, 1);
    return 0;
}

