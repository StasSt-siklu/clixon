/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
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

 * HTTP/1.1 parser according to RFC 7230
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <syslog.h>
#include <errno.h>
#include <signal.h>
#include <openssl/ssl.h>

#ifdef HAVE_LIBNGHTTP2
#include <nghttp2/nghttp2.h>
#endif

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include <clixon/clixon.h>

#include "restconf_handle.h"
#include "restconf_lib.h"
#include "restconf_root.h"
#include "restconf_native.h"
#include "restconf_api.h"
#include "restconf_err.h"
#include "clixon_http1_parse.h"
#include "restconf_http1.h"

/* Size of xml read buffer */
#define BUFLEN 1024  

/*! HTTP/1 parsing function. Input is string and side-effect is populating connection structs
 *
 * @param[in]  h        Clixon handle
 * @param[in]  rc       Restconf connection
 * @param[in]  str      Pointer to string containing HTTP/1 
 * @param[in]  filename Debug string identifying file or connection
 * @retval     0        Parse OK 
 * @retval    -1        Error with clicon_err called. 
 */
static int 
_http1_parse(clicon_handle  h,
	     restconf_conn *rc,
	     char          *str,
	     const char    *filename)
{
    int               retval = -1;
    clixon_http1_yacc hy = {0,};
    int               ret;

    clicon_debug(1, "%s:\n%s", __FUNCTION__, str);
    if (strlen(str) == 0)
	goto ok;
    hy.hy_parse_string = str;
    hy.hy_name = filename;
    hy.hy_h = h;
    hy.hy_rc = rc;
    hy.hy_linenum = 1;
    if (http1_scan_init(&hy) < 0)
	goto done;
    if (http1_parse_init(&hy) < 0)
	goto done;
    ret = clixon_http1_parseparse(&hy); /* yacc returns 1 on error */
    /* yacc/lex terminates parsing after headers. 
     * Look for body after headers assuming str terminating with \n\n\0 and then <body> */
    http1_parse_exit(&hy);
    http1_scan_exit(&hy);
    if (ret != 0){
	if (filename)
	    clicon_log(LOG_NOTICE, "HTTP1 error: on line %d in %s", hy.hy_linenum, filename);
	else
	    clicon_log(LOG_NOTICE, "HTTP1 error: on line %d", hy.hy_linenum);
	if (clicon_errno == 0)
	    clicon_err(OE_RESTCONF, 0, "HTTP1 parser error with no error code (should not happen)");
	goto done;
    }
 ok:
    retval = 0;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    return retval;
}

/*! HTTP/1 parsing function from file
 *
 * @param[in]  h        Clixon handle
 * @param[in]  rc       Restconf connection
 * @param[in]  f        A file descriptor containing HTTP/1 (as ASCII characters)
 * @param[in]  filename Debug string identifying file or connection
 * @retval     0        Parse OK 
 * @retval    -1        Error with clicon_err called. 
 */
int
clixon_http1_parse_file(clicon_handle  h,
			restconf_conn *rc,
			FILE          *f,
			const char    *filename)
{
    int   retval = -1;
    int   ret;
    char  ch;
    char *buf = NULL;
    char *ptr;
    int   buflen = BUFLEN; /* start size */
    int   len = 0;
    int   oldbuflen;

    clicon_debug(1, "%s %s", __FUNCTION__, filename);
    if (f == NULL){
	clicon_err(OE_RESTCONF, EINVAL, "f is NULL");
	goto done;
    }
    if ((buf = malloc(buflen)) == NULL){
	clicon_err(OE_XML, errno, "malloc");
	goto done;
    }
    memset(buf, 0, buflen);
    ptr = buf;
    while (1){
	if ((ret = fread(&ch, 1, 1, f)) < 0){
	    clicon_err(OE_XML, errno, "read");
	    break;
	}
	if (ret != 0){
	    buf[len++] = ch;
	}
	if (ret == 0) { /* buffer read */
	    if (_http1_parse(h, rc, ptr, filename) < 0)
		goto done;
	    break;
	}
	if (len >= buflen-1){ /* Space: one for the null character */
	    oldbuflen = buflen;
	    buflen *= 2;
	    if ((buf = realloc(buf, buflen)) == NULL){
		clicon_err(OE_XML, errno, "realloc");
		goto done;
	    }
	    memset(buf+oldbuflen, 0, buflen-oldbuflen);
	    ptr = buf;
	}
    } /* while */
    retval = 0;
 done:
    if (buf)
	free(buf);
    return retval;
}

/*! HTTP/1 parsing function from string
 *
 * @param[in]  h        Clixon handle
 * @param[in]  rc       Restconf connection
 * @param[in]  str      HTTP/1 string
 * @retval     0        Parse OK 
 * @retval    -1        Error with clicon_err called. 
 */
int 
clixon_http1_parse_string(clicon_handle  h,
			  restconf_conn *rc,
			  char          *str)
{
    return _http1_parse(h, rc, str, "http1-parse");
}

/*! HTTP/1 parsing function from buffer (non-null terminated)
 *
 * Convert buffer to null-terminated string
 * @param[in]  h        Clixon handle
 * @param[in]  rc       Restconf connection
 * @param[in]  buf      HTTP/1 buffer
 * @param[in]  n        Length of buffer
 * @retval     0        Parse OK 
 * @retval    -1        Error with clicon_err called. 
 * @note  Had preferred to do this without copying, OR 
 * input flex with a non-null terminated string
 */
int 
clixon_http1_parse_buf(clicon_handle  h,
		       restconf_conn *rc,
		       char          *buf,
		       size_t         n)
{
    char *str = NULL;
    int   ret;
    
    if ((str = malloc(n+1)) == NULL){
	clicon_err(OE_RESTCONF, errno, "malloc");
	return -1;
    }
    memcpy(str, buf, n);
    str[n] = '\0';
    ret = _http1_parse(h, rc, str, "http1-parse");
    free(str);
    return ret;
}

#ifdef HAVE_LIBNGHTTP2
/*! Check http/1 UPGRADE to http/2
 * If upgrade headers are encountered AND http/2 is configured, then 
 * - add upgrade headers or signal error
 * - set http2 flag get settings to and signal to upper layer to do the actual transition.
 * @retval   -1   Error
 * @retval    0   Yes, upgrade dont proceed with request
 * @retval    1   No upgrade, proceed with request
 * @note currently upgrade header is checked always if nghttp2 is configured but may be a 
 *       runtime config option
 */
static int
http1_upgrade_http2(clicon_handle         h,
		    restconf_stream_data *sd)
{
    int    retval = -1;
    char  *str;
    char  *settings;
    cxobj *xerr = NULL;
	
    if ((str = restconf_param_get(h, "HTTP_UPGRADE")) != NULL &&
	clicon_option_bool(h, "CLICON_RESTCONF_HTTP2_PLAIN") == 1){
	/* Only accept "h2c" */
	if (strcmp(str, "h2c") != 0){
	    if (netconf_invalid_value_xml(&xerr, "protocol", "Invalid upgrade token") < 0)
		goto done; 
	    if (api_return_err0(h, sd, xerr, 1, YANG_DATA_JSON, 0) < 0)
		goto done;
	    if (xerr)
		xml_free(xerr);
	}
	else {
	    if (restconf_reply_header(sd, "Connection", "Upgrade") < 0)
		goto done;
	    if (restconf_reply_header(sd, "Upgrade", "h2c") < 0)
		goto done;
	    if (restconf_reply_send(sd, 101, NULL, 0) < 0) /* Switch protocol */
		goto done;
	    /* Signal http/2 upgrade to http/2 to upper restconf_connection handling */
	    sd->sd_upgrade2 = 1;
	    if ((settings = restconf_param_get(h, "HTTP_HTTP2_Settings")) != NULL &&
		(sd->sd_settings2 = (uint8_t*)strdup(settings)) == NULL){
		clicon_err(OE_UNIX, errno, "strdup");
		goto done;
	    }
	}
	retval = 0; /* Yes, upgrade or error */
    }
    else
	retval = 1; /* No upgrade, proceed with request */
 done:
    return retval;
}
#endif /* HAVE_LIBNGHTTP2 */

/*! Construct an HTTP/1 reply (dont actually send it)
 */
static int
restconf_http1_reply(restconf_conn        *rc,
		     restconf_stream_data *sd)
{
    int     retval = -1;
    cg_var *cv;

    /* If body, add a content-length header 
     *    A server MUST NOT send a Content-Length header field in any response
     * with a status code of 1xx (Informational) or 204 (No Content).  A
     * server MUST NOT send a Content-Length header field in any 2xx
     * (Successful) response to a CONNECT request (Section 4.3.6 of
     * [RFC7231]).
     */
    if (sd->sd_code != 204 && sd->sd_code > 199)
	if (restconf_reply_header(sd, "Content-Length", "%zu", sd->sd_body_len) < 0)
	    goto done;	
    /* Create reply and write headers */
#if 0 /* XXX need some keep-alive logic here */
    /* protocol is HTTP/1.0 and clients wants to keep established */
    if (restconf_reply_header(sd, "Connection", "keep-alive") < 0)
	goto done;
#endif
    cprintf(sd->sd_outp_buf, "HTTP/%u.%u %u %s\r\n",
	    rc->rc_proto_d1,
	    rc->rc_proto_d2,
	    sd->sd_code,
	    restconf_code2reason(sd->sd_code));
    /* Loop over headers */
    cv = NULL;
    while ((cv = cvec_each(sd->sd_outp_hdrs, cv)) != NULL)
	cprintf(sd->sd_outp_buf, "%s: %s\r\n", cv_name_get(cv), cv_string_get(cv));
    cprintf(sd->sd_outp_buf, "\r\n");
    /* Write a body */
    if (sd->sd_body){
	cbuf_append_str(sd->sd_outp_buf, cbuf_get(sd->sd_body));
    }
    retval = 0;
 done:
    return retval;
}

/*!
 * @param[in]  h    Clixon handle
 * @param[in]  rc   Clixon request connect pointer
 */
int
restconf_http1_path_root(clicon_handle  h,
			 restconf_conn *rc)
{
    int                   retval = -1;
    restconf_stream_data *sd;
    cvec                 *cvv = NULL;
    char                 *cn;
    char                 *subject = NULL;
    cxobj                *xerr = NULL;
    int                   pretty;
    int                   ret;
    
    clicon_debug(1, "------------");
    pretty = restconf_pretty_get(h);
    if ((sd = restconf_stream_find(rc, 0)) == NULL){
	clicon_err(OE_RESTCONF, EINVAL, "No stream_data");
	goto done;
    }
    /* Sanity check */
    if (restconf_param_get(h, "REQUEST_URI") == NULL){
	if (netconf_invalid_value_xml(&xerr, "protocol", "Missing REQUEST_URI ") < 0)
	    goto done;
	/* Select json as default since content-type header may not be accessible yet */
	if (api_return_err0(h, sd, xerr, pretty, YANG_DATA_JSON, 0) < 0)
	    goto done;
	goto fail;
    }
    if ((rc->rc_proto != HTTP_10 && rc->rc_proto != HTTP_11) ||
	rc->rc_proto_d1 != 1 ||
	(rc->rc_proto_d2 != 0 && rc->rc_proto_d2 != 1)){
	if (netconf_invalid_value_xml(&xerr, "protocol", "Invalid HTTP version number") < 0)
	    goto done;
	/* Select json as default since content-type header may not be accessible yet */
	if (api_return_err0(h, sd, xerr, pretty, YANG_DATA_JSON, 0) < 0)
	    goto done;
	goto fail;
    }
    if ((sd->sd_path = restconf_uripath(rc->rc_h)) == NULL)
	goto done; // XXX SHOULDNT EXIT if no REQUEST_URI
    if (rc->rc_proto_d2 == 0 && rc->rc_proto == HTTP_11)
	rc->rc_proto = HTTP_10;
    else if (rc->rc_proto_d2 == 1 && rc->rc_proto != HTTP_10)
	rc->rc_proto = HTTP_11;
    if (rc->rc_ssl != NULL){
	/* Slightly awkward way of taking SSL cert subject and CN and add it to restconf parameters
	 * instead of accessing it directly 
	 * SSL subject fields, eg CN (Common Name) , can add more here? */
	if (ssl_x509_name_oneline(rc->rc_ssl, &subject) < 0)
	    goto done;
	if (subject != NULL) {
	    if (uri_str2cvec(subject, '/', '=', 1, &cvv) < 0)
		goto done;
	    if ((cn = cvec_find_str(cvv, "CN")) != NULL){
		if (restconf_param_set(h, "SSL_CN", cn) < 0)
		    goto done;
	    }
	}
    }
    /* Check sanity of session, eg ssl client cert validation, may set rc_exit */
    if (restconf_connection_sanity(h, rc, sd) < 0)
	goto done;
#ifdef HAVE_LIBNGHTTP2
    if ((ret = http1_upgrade_http2(h, sd)) < 0)
	goto done;
    if (ret == 0) /* upgrade */
	goto upgrade;
#endif
    /* call generic function */
    if (strcmp(sd->sd_path, RESTCONF_WELL_KNOWN) == 0){
	if (api_well_known(h, sd) < 0)
	    goto done;
    }
    else if (api_root_restconf(h, sd, sd->sd_qvec) < 0)
	goto done;
 fail:
   if (restconf_param_del_all(h) < 0)
	goto done;
 upgrade:
    if (sd->sd_code)
	if (restconf_http1_reply(rc, sd) < 0)
	    goto done;
    retval = 0;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    if (xerr)
	xml_free(xerr);
    if (cvv)
	cvec_free(cvv);
    return retval;
}

/*! Check expect header, if found generate a Continue reply
 *
 * @param[in] h   Clixon handle
 * @param[in] rc  Restconf connection
 * @param[in] sd  Restconf stream data (for http1 only stream 0)
 * @retval    1   OK, Send continue 
 * @retval    0   OK, Dont send continue
 * @retval   -1   Error
 * @see rfc7231 Sec 5.1.1
 */
int
http1_check_expect(clicon_handle         h,
		   restconf_conn        *rc,
		   restconf_stream_data *sd)
{
    int    retval = -1;
    char  *val;
    
    if ((val = restconf_param_get(h, "HTTP_EXPECT")) != NULL &&
	strcmp(val, "100-continue") == 0){ /* just drop if not well-formed */
	sd->sd_code = 100;
	if (restconf_http1_reply(rc, sd) < 0)
	    goto done;
	retval = 1; /* send continue by flushing stream buffer after the call */
    }
    else
	retval = 0;
 done:
    return retval;
}

/*! Is there more data to be read?
 *
 * Use Content-Length header as an indicator on the status of reading an input message:
 * 0: No Content-Length or 0
 *    Either message header not fully read OR header does not contain Content-Length
 * 1: Content-Length found but body has fewer bytes, ie remaining bytes to read
 * 2: Content-Length found and matches body length. No more bytes to read
 * @param[in]  h       Clixon handle
 * @param[in]  sd      Restconf stream data (for http1 only stream 0)
 * @param[out] status  0-2, see description above
 * @retval     0       OK, see status param
 * @retval    -1       Error
 */
int
http1_check_content_length(clicon_handle         h,
			   restconf_stream_data *sd,
			   int                  *status)
{
    int   retval = -1;
    char *val;
    int   len;
    
    if ((val = restconf_param_get(h, "HTTP_CONTENT_LENGTH")) == NULL ||
	(len = atoi(val)) == 0)
	*status = 0;
    else{
	if (cbuf_len(sd->sd_indata) < len)
	    *status = 1;
	else
	    *status = 2;
    }
    retval = 0;
    return retval;
}