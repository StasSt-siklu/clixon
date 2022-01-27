/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
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
#include <unistd.h>
#include <string.h>
#include <syslog.h>
#include <pwd.h>
#include <ctype.h>
#include <assert.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/resource.h>

#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#ifdef HAVE_LIBNGHTTP2
#include <nghttp2/nghttp2.h>
#endif

/* cligen */
#include <cligen/cligen.h>

/* libclixon */
#include <clixon/clixon.h>

/* restconf */
#include "restconf_lib.h"       /* generic shared with plugins */
#include "restconf_handle.h"
#include "restconf_err.h"
#include "restconf_native.h"    /* Restconf-openssl mode specific headers*/
#ifdef HAVE_LIBNGHTTP2
#include <nghttp2/nghttp2.h>
#include "restconf_nghttp2.h"  /* http/2 */
#endif
#ifdef HAVE_HTTP1
#include "restconf_http1.h"
#endif

/*!
 * @see restconf_stream_free
 */
restconf_stream_data *
restconf_stream_data_new(restconf_conn *rc,
			 int32_t        stream_id)
{
    restconf_stream_data *sd;

    if ((sd = malloc(sizeof(restconf_stream_data))) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	return NULL;
    }
    memset(sd, 0, sizeof(restconf_stream_data));
    sd->sd_stream_id = stream_id;
    sd->sd_fd = -1;
    if ((sd->sd_indata = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	return NULL;
    }
    if ((sd->sd_outp_hdrs = cvec_new(0)) == NULL){
	clicon_err(OE_UNIX, errno, "cvec_new");
	return NULL;
    }
    if ((sd->sd_outp_buf = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	return NULL;
    }
    sd->sd_conn = rc;
    INSQ(sd, rc->rc_streams);
    return sd;
}

restconf_stream_data *
restconf_stream_find(restconf_conn *rc,
		     int32_t        id)
{
    restconf_stream_data *sd;

    if ((sd = rc->rc_streams) != NULL) {
	do {
	    if (sd->sd_stream_id == id)
		return sd;
	    sd = NEXTQ(restconf_stream_data *, sd);
	} while (sd && sd != rc->rc_streams);
    }
    return NULL;
}

int
restconf_stream_free(restconf_stream_data *sd)
{
    if (sd->sd_fd != -1) {
	close(sd->sd_fd);
    }
    if (sd->sd_indata)
	cbuf_free(sd->sd_indata);
    if (sd->sd_outp_hdrs)
	cvec_free(sd->sd_outp_hdrs);
    if (sd->sd_outp_buf)
	cbuf_free(sd->sd_outp_buf);
    if (sd->sd_body)
	cbuf_free(sd->sd_body);
    if (sd->sd_path)
	free(sd->sd_path);
    if (sd->sd_settings2)
	free(sd->sd_settings2);
    if (sd->sd_qvec)
	cvec_free(sd->sd_qvec);
    free(sd);
    return 0;
}

/*! Create restconf connection struct
 */
restconf_conn *
restconf_conn_new(clicon_handle h,
		  int           s)
{
    restconf_conn *rc;

    if ((rc = (restconf_conn*)malloc(sizeof(restconf_conn))) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	return NULL;
    }
    memset(rc, 0, sizeof(restconf_conn));
    rc->rc_h = h;
    rc->rc_s = s;
    return rc;
}

/*! Free clixon/cbuf resources related to a connection
 * @param[in]  rc   restconf connection
 */
int
restconf_conn_free(restconf_conn *rc)
{
    restconf_stream_data *sd;
    
    if (rc == NULL){
	clicon_err(OE_RESTCONF, EINVAL, "rc is NULL");
	return -1;
    }
#ifdef HAVE_LIBNGHTTP2
    if (rc->rc_ngsession)
	nghttp2_session_del(rc->rc_ngsession);
#endif
    /* Free all streams */
    while ((sd = rc->rc_streams) != NULL) {
	DELQ(sd, rc->rc_streams,  restconf_stream_data *);
	if (sd)
	    restconf_stream_free(sd);
    }
    free(rc);
    return 0;
}

/*! Given SSL connection, get peer certificate one-line name
 * @param[in]  ssl      SSL session
 * @param[out] oneline  Cert name one-line
 */
int
ssl_x509_name_oneline(SSL   *ssl,
		      char **oneline)
{
    int        retval = -1;
    char      *p = NULL;
    X509      *cert = NULL;
    X509_NAME *name;

    if (ssl == NULL || oneline == NULL) {
	clicon_err(OE_RESTCONF, EINVAL, "ssl or cn is NULL");
	goto done;
    }
    if ((cert = SSL_get_peer_certificate(ssl)) == NULL)
	goto ok;
    if ((name = X509_get_subject_name(cert)) == NULL) 
	goto ok;
    if ((p = X509_NAME_oneline(name, NULL, 0)) == NULL)
	goto ok;
    if ((*oneline = strdup(p)) == NULL){
	clicon_err(OE_UNIX, errno, "strdup");
	goto done;
    }
 ok:
    retval = 0;
 done:
    if (p)
	OPENSSL_free(p);
    if (cert)
	X509_free(cert);
    return retval;
}

/*! Check common connection sanity checks and terminate if found before request processing
 *
 * Tests of sanity of connection not really of an individual request, but is triggered by
 * the (first) request in http/1 and http/2
 * These tests maybe could have done earlier, this is somewhat late since the session is
 * closed and that is always good to do as early as possible.
 * The following are current checks:
 * 1) Check if http/2 non-tls is disabled
 * 2) Check if ssl client certs ae valid
 * @param[in]  h     Clixon handle
 * @param[in]  rc    Restconf connection handle 
 * @param[in]  sd    Http stream
 * @param[out] term  Terminate session
 * @retval    -1     Error
 * @retval     0     OK
 */
int
restconf_connection_sanity(clicon_handle         h,
			   restconf_conn        *rc,
			   restconf_stream_data *sd)
{
    int            retval = -1;
    cxobj         *xerr = NULL;
    long           code;
    cbuf          *cberr = NULL;
    restconf_media media_out = YANG_DATA_JSON;
    char          *media_str = NULL;
    
    /* 1) Check if http/2 non-tls is disabled */
    if (rc->rc_ssl == NULL &&
	rc->rc_proto == HTTP_2 &&
	clicon_option_bool(h, "CLICON_RESTCONF_HTTP2_PLAIN") == 0){
	if (netconf_invalid_value_xml(&xerr, "protocol", "Only HTTP/2 with TLS is enabled, plain http/2 is disabled") < 0)
	    goto done;
	if ((media_str = restconf_param_get(h, "HTTP_ACCEPT")) == NULL){
	     media_out = YANG_DATA_JSON;
	}
	else if ((int)(media_out = restconf_media_str2int(media_str)) == -1){
	    if (strcmp(media_str, "*/*") == 0) /* catch-all */
		media_out = YANG_DATA_JSON;
	}
	if (api_return_err0(h, sd, xerr, 1, media_out, 0) < 0)
	    goto done;
	rc->rc_exit = 1;
    }
    /* 2) Check if ssl client cert is valid */
    else if (rc->rc_ssl != NULL &&
	     (code = SSL_get_verify_result(rc->rc_ssl)) != 0){
	if ((cberr = cbuf_new()) == NULL){
	    clicon_err(OE_UNIX, errno, "cbuf_new");
	    goto done;
	}
	cprintf(cberr, "HTTP cert verification failed, unknown ca: (code:%ld)", code); 
	if (netconf_invalid_value_xml(&xerr, "protocol", cbuf_get(cberr)) < 0)
	    goto done;
	if ((media_str = restconf_param_get(h, "HTTP_ACCEPT")) == NULL){
	     media_out = YANG_DATA_JSON;
	}
	else if ((int)(media_out = restconf_media_str2int(media_str)) == -1){
	    if (strcmp(media_str, "*/*") == 0) /* catch-all */
		media_out = YANG_DATA_JSON;
	}
	if (api_return_err0(sd->sd_conn->rc_h, sd, xerr, 1, media_out, 0) < 0)
	    goto done;
	rc->rc_exit = 1;
    }
    retval = 0;
 done:
    if (cberr)
	cbuf_free(cberr);
    if (xerr)
	xml_free(xerr);
    return retval;
}

/* Write buf to socket
 * see also this function in restcont_api_openssl.c
 */
static int
native_buf_write(char   *buf,
		 size_t  buflen,
		 int     s,
		 SSL    *ssl)
{
    int     retval = -1;
    ssize_t len;
    ssize_t totlen = 0;
    int     er;

    /* Two problems with debugging buffers that this fixes:
     * 1. they are not "strings" in the sense they are not NULL-terminated
     * 2. they are often very long
     */
    if (clicon_debug_get()) { 
	char *dbgstr = NULL;
	size_t sz;
	sz = buflen>256?256:buflen; /* Truncate to 256 */
	if ((dbgstr = malloc(sz+1)) == NULL){
	    clicon_err(OE_UNIX, errno, "malloc");
	    goto done;
	}
	memcpy(dbgstr, buf, sz);
	dbgstr[sz] = '\0';
	clicon_debug(1, "%s buflen:%zu buf:\n%s", __FUNCTION__, buflen, dbgstr);
	free(dbgstr);
    }
    while (totlen < buflen){
	if (ssl){
	    if ((len = SSL_write(ssl, buf+totlen, buflen-totlen)) <= 0){
		er = errno;
		switch (SSL_get_error(ssl, len)){
		case SSL_ERROR_SYSCALL:              /* 5 */
		    if (er == ECONNRESET) {/* Connection reset by peer */
			if (ssl)
			    SSL_free(ssl);
			close(s);
			clixon_event_unreg_fd(s, restconf_connection);
			goto ok; /* Close socket and ssl */
		    }
		    else if (er == EAGAIN){
			clicon_debug(1, "%s write EAGAIN", __FUNCTION__);
			usleep(10000);
			continue;
		    }
		    else{
			clicon_err(OE_RESTCONF, er, "SSL_write %d", er);
			goto done;
		    }
		    break;
		default:
		    clicon_err(OE_SSL, 0, "SSL_write");
		    goto done;
		    break;
		}
		goto done;
	    }
	}
	else{
	    if ((len = write(s, buf+totlen, buflen-totlen)) < 0){
		switch (errno){
		case EAGAIN:     /* Operation would block */
		    clicon_debug(1, "%s write EAGAIN", __FUNCTION__);
		    usleep(10000);
		    continue;
		    break;
		case ECONNRESET: /* Connection reset by peer */
		case EPIPE:   /* Broken pipe */
		    close(s);
		    clixon_event_unreg_fd(s, restconf_connection);
		    goto ok; /* Close socket and ssl */
		    break;
		default:
		    clicon_err(OE_UNIX, errno, "write %d", errno);
		    goto done;
		    break;
		}
	    }
	    assert(len != 0);
	}
	totlen += len;
    } /* while */
 ok:
    retval = 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    return retval;
}

/*! Send early handcoded bad request reply before actual packet received, just after accept
 * @param[in]  h    Clixon handle
 * @param[in]  s    Socket
 * @param[in]  ssl  If set, it will be freed
 * @param[in]  body If given add message body using media 
 * @see restconf_badrequest which can only be called in a request context
 */
int
native_send_badrequest(clicon_handle       h,
		       int                 s,
		       SSL                *ssl,
		       char               *media,
		       char               *body)
{
    int retval = -1;
    cbuf *cb = NULL;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	goto done;
    }
    cprintf(cb, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n");
    if (body){
	cprintf(cb, "Content-Type: %s\r\n", media);
	cprintf(cb, "Content-Length: %zu\r\n", strlen(body)+2); /* for \r\n */
    }
    else
	cprintf(cb, "Content-Length: 0\r\n");
    cprintf(cb, "\r\n");
    if (body)
	cprintf(cb, "%s\r\n", body);
    if (native_buf_write(cbuf_get(cb), cbuf_len(cb), s, ssl) < 0)
	goto done;
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! New data connection after accept, receive and reply on data socket
 *
 * @param[in]   s    Socket where message arrived. read from this.
 * @param[in]   arg  Client entry (from).
 * @retval      0    OK
 * @retval      -1   Error Terminates backend and is never called). Instead errors are
 *                   propagated back to client.
 * @see restconf_accept_client where this callback is registered
 * @note read buffer is limited. More data can be read in two ways:  returns a buffer
 * with 100 Continue, in which case that is replied and the function returns and the client sends 
 * more data.
 * OR  returns 0 with no reply, then this is assumed to mean read more data from the socket.
 */
int
restconf_connection(int   s,
		    void *arg)
{
    int                   retval = -1;
    restconf_conn        *rc = NULL;
    ssize_t               n;
    char                  buf[BUFSIZ]; /* from stdio.h, typically 8K. 256 fails some tests*/
    char                 *totbuf = NULL;
    size_t                totlen = 0;
    int                   readmore = 1;
    int                   sslerr;
    int                   contnr = 0; /* Continue sent */
#ifdef HAVE_LIBNGHTTP2
    int                   ret;
#endif
#ifdef HAVE_HTTP1
    clicon_handle         h;
    restconf_stream_data *sd;
#endif

    clicon_debug(1, "%s %d", __FUNCTION__, s);
    if ((rc = (restconf_conn*)arg) == NULL){
	clicon_err(OE_RESTCONF, EINVAL, "arg is NULL");
	goto done;
    }
    assert(s == rc->rc_s);
    while (readmore) {
	clicon_debug(1, "%s readmore", __FUNCTION__);
	readmore = 0;
	/* Example: curl -Ssik -u wilma:bar -X GET https://localhost/restconf/data/example:x */
	if (rc->rc_ssl){
	    /* Non-ssl gets n == 0 here!
	       curl -Ssik --key /var/tmp/./test_restconf_ssl_certs.sh/certs/limited.key --cert /var/tmp/./test_restconf_ssl_certs.sh/certs/limited.crt -X GET https://localhost/restconf/data/example:x
	    */
	    if ((n = SSL_read(rc->rc_ssl, buf, sizeof(buf))) < 0){
		sslerr = SSL_get_error(rc->rc_ssl, n);
		clicon_debug(1, "%s SSL_read() n:%zd errno:%d sslerr:%d", __FUNCTION__, n, errno, sslerr);
		switch (sslerr){
		case SSL_ERROR_WANT_READ:            /* 2 */
		    /* SSL_ERROR_WANT_READ is returned when the last operation was a read operation 
		     * from a nonblocking BIO. 
		     * That is, it can happen if restconf_socket_init() below is called 
		     * with SOCK_NONBLOCK
		     */
		    clicon_debug(1, "%s SSL_read SSL_ERROR_WANT_READ", __FUNCTION__);
		    usleep(1000);
		    readmore = 1;
		    break;
		default:
		    clicon_err(OE_XML, errno, "SSL_read");
		    goto done;              
		} /* switch */
		continue; /* readmore */
	    }
	}
	else{
	    if ((n = read(rc->rc_s, buf, sizeof(buf))) < 0){ /* XXX atomicio ? */
		switch(errno){
		case ECONNRESET:/* Connection reset by peer */
		    clicon_debug(1, "%s %d Connection reset by peer", __FUNCTION__, rc->rc_s);
		    clixon_event_unreg_fd(rc->rc_s, restconf_connection);
		    close(rc->rc_s);
		    restconf_conn_free(rc);
		    goto ok; /* Close socket and ssl */
		    break;
		case EAGAIN:
		    clicon_debug(1, "%s read EAGAIN", __FUNCTION__);
		    usleep(1000);
		    readmore = 1;
		    break;
		default:;
		    clicon_err(OE_XML, errno, "read");
		    goto done;
		    break;
		}
		continue;
	    }
	}
	clicon_debug(1, "%s read:%zd", __FUNCTION__, n);
	if (n == 0){
	    clicon_debug(1, "%s n=0 closing socket", __FUNCTION__);
	    if (restconf_close_ssl_socket(rc, 0) < 0)
		goto done;
	    restconf_conn_free(rc);    
	    rc = NULL;
	    goto ok;
	}
	switch (rc->rc_proto){
#ifdef HAVE_HTTP1
	case HTTP_10:
	case HTTP_11:
	    h = rc->rc_h;
	    /* default stream */
	    if ((sd = restconf_stream_find(rc, 0)) == NULL){
		clicon_err(OE_RESTCONF, EINVAL, "restconf stream not found");
		goto done;
	    }
	    /* multi-buffer for multiple reads */
	    totlen += n;
	    if ((totbuf = realloc(totbuf, totlen+1)) == NULL){
		clicon_err(OE_UNIX, errno, "realloc");
		goto done;
	    }
	    memcpy(&totbuf[totlen-n], buf, n);
	    totbuf[totlen] = '\0';
	    if (clixon_http1_parse_string(h, rc, totbuf) < 0){
		if (native_send_badrequest(h, rc->rc_s, rc->rc_ssl, "application/yang-data+xml",
				    "<errors xmlns=\"urn:ietf:params:xml:ns:yang:ietf-restconf\"><error><error-type>protocol</error-type><error-tag>malformed-message</error-tag><error-message>The requested URL or a header is in some way badly formed</error-message></error></errors>") < 0)
		    goto done;
	    }
	    else{
		/* Check for Continue and if so reply with 100 Continue 
		 * ret == 1: send reply
		 */
		if (!contnr){
		    if ((ret = http1_check_expect(h, rc, sd)) < 0)
			goto done;
		    if (ret == 1){
			if (native_buf_write(cbuf_get(sd->sd_outp_buf), cbuf_len(sd->sd_outp_buf),
					     rc->rc_s, rc->rc_ssl) < 0)
			    goto done;
			cvec_reset(sd->sd_outp_hdrs);
			cbuf_reset(sd->sd_outp_buf);
			contnr++;
		    }
		}
		/* Check whole message is read. 
		 * ret == 0: need more bytes
		 */
		if ((ret = http1_check_readmore(h, sd)) < 0)
		    goto done;
		if (ret == 0){
		    readmore++;
#if 1
		    /* Clear all stream data if reading more 
		     * Alternative would be to not adding new data to totbuf ^
		     * and just append to sd->sd_indata but that would assume 
		     * all headers read on first round. But that cant be done withut
		     * some probing on the socket if there is more data since it
		     * would hang on read otherwise
		     */
		    cbuf_reset(sd->sd_indata);
		    if (sd->sd_qvec)
			cvec_free(sd->sd_qvec);
		    if (restconf_param_del_all(h) < 0)
			goto done;
#endif
		    continue;
		}
		if (restconf_http1_path_root(h, rc) < 0)
		    goto done;
		if (native_buf_write(cbuf_get(sd->sd_outp_buf), cbuf_len(sd->sd_outp_buf),
			      rc->rc_s, rc->rc_ssl) < 0)
		    goto done;
		cvec_reset(sd->sd_outp_hdrs); /* Can be done in native_send_reply */
		cbuf_reset(sd->sd_outp_buf);
	    }
	    if (rc->rc_exit){  /* Server-initiated exit for http/2 */
		SSL_free(rc->rc_ssl);
		rc->rc_ssl = NULL;
		if (close(rc->rc_s) < 0){
		    clicon_err(OE_UNIX, errno, "close");
		    goto done;
		}
		clixon_event_unreg_fd(rc->rc_s, restconf_connection);
		restconf_conn_free(rc);
		goto ok;
	    }
#ifdef HAVE_LIBNGHTTP2
	    if (sd->sd_upgrade2){
		nghttp2_error ngerr;

		/* Switch to http/2 according to RFC 7540 Sec 3.2 and RFC 7230 Sec 6.7 */
		rc->rc_proto = HTTP_2;
		if (http2_session_init(rc) < 0){
		    restconf_close_ssl_socket(rc, 1);
		    goto done;
		}
		/* The HTTP/1.1 request that is sent prior to upgrade is assigned a
		 * stream identifier of 1 (see Section 5.1.1) with default priority
		 */
		sd->sd_stream_id = 1;
		/* The first HTTP/2 frame sent by the server MUST be a server connection
		 * preface (Section 3.5) consisting of a SETTINGS frame (Section 6.5).
		 */
		if ((ngerr = nghttp2_session_upgrade2(rc->rc_ngsession,
						      sd->sd_settings2,
						      sd->sd_settings2?strlen((const char*)sd->sd_settings2):0,
						      0, /* XXX: 1 if HEAD */
						      NULL)) < 0){
		    clicon_err(OE_NGHTTP2, ngerr, "nghttp2_session_upgrade2");
		    goto done;
		}
		if (http2_send_server_connection(rc) < 0){
		    restconf_close_ssl_socket(rc, 1);
		    goto done;
		}
		/* Use params from original http/1 session to http/2 stream */
		if (http2_exec(rc, sd, rc->rc_ngsession, 1) < 0)
		    goto done;
		/*
		 * Very special case for http/1->http/2 upgrade and restconf "restart"
		 * That is, the restconf daemon is restarted under the hood, and the session
		 * is closed in mid-step: it needs a couple of extra rounds to complete the http/2
		 * settings before it completes.
		 * Maybe a more precise way would be to encode that semantics using recieved http/2
		 * frames instead of just postponing nrof events?
		 */
		if (clixon_exit_get() == 1){
		    clixon_exit_set(3);
		}
	    }
#endif
	    break;
#endif /* HAVE_HTTP1 */
#ifdef HAVE_LIBNGHTTP2
	case HTTP_2:
	    if (rc->rc_exit){ /* Server-initiated exit for http/2 */
		nghttp2_error ngerr;
		if ((ngerr = nghttp2_session_terminate_session(rc->rc_ngsession, 0)) < 0)
		    clicon_err(OE_NGHTTP2, ngerr, "nghttp2_session_terminate_session %d", ngerr);
	    }
	    else {
		if ((ret = http2_recv(rc, (unsigned char *)buf, n)) < 0)
		    goto done;
		if (ret == 0){
		    restconf_close_ssl_socket(rc, 1);
		    if (restconf_conn_free(rc) < 0)
			goto done;
		    goto ok;
		}
		/* There may be more data frames */
		readmore++;
	    }
	    break;
#endif /* HAVE_LIBNGHTTP2 */
	default:
	    break;
	} /* switch rc_proto */
    } /* while readmore */
 ok:
    retval = 0;
 done:
    if (totbuf)
	free(totbuf);
    clicon_debug(1, "%s retval %d", __FUNCTION__, retval);
    return retval;
} /* restconf_connection */
