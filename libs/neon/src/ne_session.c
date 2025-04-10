/* 
   HTTP session handling
   Copyright (C) 1999-2021, Joe Orton <joe@manyfish.co.uk>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.
   
   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
   MA 02111-1307, USA

*/

#include "config.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#ifdef HAVE_LIBPROXY
#include <proxy.h>
#endif

#include "ne_session.h"
#include "ne_alloc.h"
#include "ne_utils.h"
#include "ne_internal.h"
#include "ne_string.h"
#include "ne_dates.h"

#include "ne_private.h"

/* Destroy a a list of hooks. */
static void destroy_hooks(struct hook *hooks)
{
    struct hook *nexthk;

    while (hooks) {
	nexthk = hooks->next;
	ne_free(hooks);
	hooks = nexthk;
    }
}

static void free_hostinfo(struct host_info *hi)
{
    if (hi->hostname) ne_free(hi->hostname);
    if (hi->hostport) ne_free(hi->hostport);
    if (hi->address) ne_addr_destroy(hi->address);
    if (hi->literal) ne_iaddr_free(hi->literal);
}

/* Destroy the sess->proxies array. */
static void free_proxies(ne_session *sess)
{
    struct host_info *hi, *nexthi;

    for (hi = sess->proxies; hi; hi = nexthi) {
        nexthi = hi->next;
        free_hostinfo(hi);
        ne_free(hi);
    }

    sess->proxies = NULL;
    sess->any_proxy_http = 0;
}

void ne_session_destroy(ne_session *sess) 
{
    NE_DEBUG_WINSCP_CONTEXT(sess);
    struct hook *hk;

    NE_DEBUG(NE_DBG_HTTP, "sess: Destroying session.\n");

    /* Run the destroy hooks. */
    for (hk = sess->destroy_sess_hooks; hk != NULL; hk = hk->next) {
	ne_destroy_sess_fn fn = (ne_destroy_sess_fn)hk->fn;
	fn(hk->userdata);
    }

    /* Close the connection; note that the notifier callback could
     * still be invoked here. */
    if (sess->connected) {
        ne_close_connection(sess);
    }
    
    destroy_hooks(sess->create_req_hooks);
    destroy_hooks(sess->pre_send_hooks);
    destroy_hooks(sess->post_headers_hooks);
    destroy_hooks(sess->post_send_hooks);
    destroy_hooks(sess->destroy_req_hooks);
    destroy_hooks(sess->destroy_sess_hooks);
    destroy_hooks(sess->close_conn_hooks);
    destroy_hooks(sess->private);

    ne_free(sess->scheme);

    free_hostinfo(&sess->server);
    free_proxies(sess);

    if (sess->user_agent) ne_free(sess->user_agent);
#ifdef WINSCP
    if (sess->realhost) ne_free(sess->realhost);
#endif
    if (sess->socks_user) ne_free(sess->socks_user);
    if (sess->socks_password) ne_free(sess->socks_password);

#ifdef NE_HAVE_SSL
    if (sess->ssl_context)
        ne_ssl_context_destroy(sess->ssl_context);

    if (sess->server_cert)
        ne_ssl_cert_free(sess->server_cert);
    
    if (sess->client_cert)
        ne_ssl_clicert_free(sess->client_cert);
#endif

    ne_free(sess);
}

int ne_version_pre_http11(ne_session *s)
{
    return !s->is_http11;
}

/* Stores the "hostname[:port]" segment */
static void set_hostport(struct host_info *host, unsigned int defaultport)
{
    if (host->port == defaultport) {
        host->hostport = ne_strdup(host->hostname);
    }
    else {
        char buf[512];

        ne_snprintf(buf, sizeof buf, "%s:%u", host->hostname, host->port);
        host->hostport = ne_strdup(buf);
    }
}

#define V6_ADDR_MINLEN strlen("[::1]") /* "[::]" never valid */
#define V6_SCOPE_SEP "%25"
#define V6_SCOPE_SEPLEN (strlen(V6_SCOPE_SEP))
/* Minimum length of link-local address with scope. */
#define V6_SCOPE_MINLEN (strlen("[fe80::%251]"))

/* Stores the hostname/port in *HI, setting up the "hostport" segment
 * correctly. RFC 6874 syntax is allowed here but the scope ID is
 * stripped from the hostname which is used in the Host header.  RFC
 * 9110's Host header uses uri-host, which references RFC 3986 and not
 * RFC 6874, so it is pedantically correct; the scope ID also has no
 * possible interpretation outside of the client host.
 *
 * TODO: This function also does not propagate parse failures or scope
 * mapping failures, which is bad. */
static void set_hostinfo(struct host_info *hi, enum proxy_type type, 
                         const char *hostname, unsigned int port)
{
    ne_inet_addr *ia;
    size_t hlen;

    hi->hostname = ne_strdup(hostname);
    hi->port = port;
    hi->proxy = type;

    hlen = strlen(hi->hostname);

    /* IP literal parsing. */
    ia = ne_iaddr_parse(hi->hostname, ne_iaddr_ipv4);
    if (!ia && hlen >= V6_ADDR_MINLEN
        && hi->hostname[0] == '[' && hi->hostname[hlen-1] == ']') {
        const char *v6end, *v6start = hi->hostname + 1;
        char *v6lit, *scope = NULL;

        /* Parse here, see if there is a Zone ID:
         *  IPv6addrzb => v6start = IPv6address "%25" ZoneID */

        if (hlen >= V6_SCOPE_MINLEN
            && (scope = strstr(v6start, V6_SCOPE_SEP)) != NULL)
            v6end = scope;
        else
            v6end = hi->hostname + hlen - 1; /* trailing ']' */

        /* Extract the IPv6-literal part. */
        v6lit = ne_strndup(v6start, v6end - v6start);
        ia = ne_iaddr_parse(v6lit, ne_iaddr_ipv6);
        if (ia && scope) {
            /* => scope = "%25" scope  "]" */
            char *v6scope = ne_strndup(scope + V6_SCOPE_SEPLEN,
                                       strlen(scope) - (V6_SCOPE_SEPLEN + 1));

            if (ne_iaddr_set_scope(ia, v6scope) == 0) {
                /* Strip scope from hostname since it's used in Host:
                 * headers and will be rejected. This is safe since
                 * strlen(scope) is assured by strstr() above. */
                *scope++ = ']';
                *scope = '\0';
                NE_DEBUG(NE_DBG_HTTP, "sess: Using IPv6 scope '%s', "
                         "hostname rewritten to %s.\n", v6scope,
                         hi->hostname);
            }
            else {
                NE_DEBUG(NE_DBG_HTTP, "sess: Failed to set IPv6 scope '%s' "
                         "for address %s.\n", v6scope, v6lit);
            }

            ne_free(v6scope);
        }

        ne_free(v6lit);
    }

    if (ia) {
        NE_DEBUG(NE_DBG_HTTP, "sess: Using IP literal address for %s\n",
                 hostname);
        hi->network = hi->literal = ia;
    }
}

ne_session *ne_session_create(const char *scheme,
			      const char *hostname, unsigned int port)
{
    ne_session *sess = ne_calloc(sizeof *sess);

    NE_DEBUG(NE_DBG_HTTP, "HTTP session to %s://%s:%d begins.\n",
	     scheme, hostname, port);

    ne_strnzcpy(sess->error, _("Unknown error."), sizeof sess->error);

    /* use SSL if scheme is https */
    sess->use_ssl = !strcmp(scheme, "https");
    
    /* set the hostname/port */
    set_hostinfo(&sess->server, PROXY_NONE, hostname, port);
    set_hostport(&sess->server, sess->use_ssl?443:80);

#ifdef NE_HAVE_SSL
    if (sess->use_ssl) {
        sess->ssl_context = ne_ssl_context_create(0);
        
        if (!sess->server.literal) {
            sess->flags[NE_SESSFLAG_TLS_SNI] = 1;
        }
        NE_DEBUG(NE_DBG_SSL, "ssl: SNI %s by default.\n",
                 sess->flags[NE_SESSFLAG_TLS_SNI] ?
                 "enabled" : "disabled");
    }
#endif

    sess->scheme = ne_strdup(scheme);

    /* Set flags which default to on: */
    sess->flags[NE_SESSFLAG_PERSIST] = 1;
    sess->flags[NE_SESSFLAG_STRICT] = 1;

#ifdef NE_ENABLE_AUTO_LIBPROXY
    ne_session_system_proxy(sess, 0);
#endif

    return sess;
}

void ne_session_proxy(ne_session *sess, const char *hostname,
		      unsigned int port)
{
    free_proxies(sess);

    sess->proxies = ne_calloc(sizeof *sess->proxies);

    sess->any_proxy_http = 1;
    
    set_hostinfo(sess->proxies, PROXY_HTTP, hostname, port);
}

void ne_session_socks_proxy(ne_session *sess, enum ne_sock_sversion vers, 
                            const char *hostname, unsigned int port,
                            const char *username, const char *password)
{
    free_proxies(sess);

    sess->proxies = ne_calloc(sizeof *sess->proxies);

    set_hostinfo(sess->proxies, PROXY_SOCKS, hostname, port);

    sess->socks_ver = vers;

    if (username) sess->socks_user = ne_strdup(username);
    if (password) sess->socks_password = ne_strdup(password);
}

void ne_session_system_proxy(ne_session *sess, unsigned int flags)
{
    NE_DEBUG_WINSCP_CONTEXT(sess);
#ifdef HAVE_LIBPROXY
    pxProxyFactory *pxf = px_proxy_factory_new();
    struct host_info *hi, **lasthi;
    char *url, **proxies;
    ne_uri uri;
    unsigned n;

    free_proxies(sess);

    /* Create URI for session to pass off to libproxy */
    memset(&uri, 0, sizeof uri);
    ne_fill_server_uri(sess, &uri);

    uri.path = "/"; /* make valid URI structure. */
    url = ne_uri_unparse(&uri);
    uri.path = NULL;

    /* Get list of pseudo-URIs from libproxy: */
    proxies = px_proxy_factory_get_proxies(pxf, url);
    
    for (n = 0, lasthi = &sess->proxies; proxies[n]; n++) {
        enum proxy_type ptype;

        ne_uri_free(&uri);

        NE_DEBUG(NE_DBG_HTTP, "sess: libproxy #%u=%s\n", 
                 n, proxies[n]);

        if (ne_uri_parse(proxies[n], &uri))
            continue;
        
        if (!uri.scheme) continue;

        if (ne_strcasecmp(uri.scheme, "http") == 0)
            ptype = PROXY_HTTP;
        else if (ne_strcasecmp(uri.scheme, "socks") == 0)
            ptype = PROXY_SOCKS;
        else if (ne_strcasecmp(uri.scheme, "direct") == 0)
            ptype = PROXY_NONE;
        else
            continue;

        /* Hostname/port required for http/socks schemes. */
        if (ptype != PROXY_NONE && !(uri.host && uri.port))
            continue;
        
        /* Do nothing if libproxy returned only a single "direct://"
         * entry -- a single "direct" (noop) proxy is equivalent to
         * having none. */
        if (n == 0 && proxies[1] == NULL && ptype == PROXY_NONE)
            break;

        NE_DEBUG(NE_DBG_HTTP, "sess: Got proxy %s://%s:%d\n",
                 uri.scheme, uri.host ? uri.host : "(none)",
                 uri.port);
        
        hi = *lasthi = ne_calloc(sizeof *hi);
        
        if (ptype == PROXY_NONE) {
            /* A "direct" URI requires an attempt to connect directly to
             * the origin server, so dup the server details. */
            set_hostinfo(hi, ptype, sess->server.hostname,
                         sess->server.port);
        }
        else {
            /* SOCKS/HTTP proxy. */
            set_hostinfo(hi, ptype, uri.host, uri.port);

            if (ptype == PROXY_HTTP)
                sess->any_proxy_http = 1;
            else if (ptype == PROXY_SOCKS)
                sess->socks_ver = NE_SOCK_SOCKSV5;
        }

        lasthi = &hi->next;
    }

    /* Free up the proxies array: */
    for (n = 0; proxies[n]; n++)
        free(proxies[n]);
    free(proxies[n]);

    ne_free(url);
    ne_uri_free(&uri);
    px_proxy_factory_free(pxf);
#endif
}

void ne_set_addrlist2(ne_session *sess, unsigned int port,
                      const ne_inet_addr **addrs, size_t n)
{
    struct host_info *hi, **lasthi;
    size_t i;

    free_proxies(sess);

    lasthi = &sess->proxies;

    for (i = 0; i < n; i++) {
        *lasthi = hi = ne_calloc(sizeof *hi);
        
        hi->proxy = PROXY_NONE;
        hi->network = addrs[i];
        hi->port = port;

        lasthi = &hi->next;
    }
}

void ne_set_addrlist(ne_session *sess, const ne_inet_addr **addrs, size_t n)
{
    ne_set_addrlist2(sess, sess->server.port, addrs, n);
}

void ne_set_localaddr(ne_session *sess, const ne_inet_addr *addr)
{
    sess->local_addr = addr;    
}

void ne_set_error(ne_session *sess, const char *format, ...)
{
    va_list params;

    va_start(params, format);
    ne_vsnprintf(sess->error, sizeof sess->error, format, params);
    va_end(params);
}

void ne_set_session_flag(ne_session *sess, ne_session_flag flag, int value)
{
    if (flag < NE_SESSFLAG_LAST) {
        sess->flags[flag] = value;
    }
}

int ne_get_session_flag(ne_session *sess, ne_session_flag flag)
{
    if (flag < NE_SESSFLAG_LAST) {
        return sess->flags[flag];
    }
    return -1;
}

static void progress_notifier(void *userdata, ne_session_status status,
                              const ne_session_status_info *info)
{
    ne_session *sess = userdata;

    if (status == ne_status_sending || status == ne_status_recving) {
        sess->progress_cb(sess->progress_ud, info->sr.progress, info->sr.total);    
    }
}

void ne_set_progress(ne_session *sess, ne_progress progress, void *userdata)
{
    if (progress) {
        sess->progress_cb = progress;
        sess->progress_ud = userdata;
        ne_set_notifier(sess, progress_notifier, sess);
    }
    else {
        ne_set_notifier(sess, NULL, NULL);
    }
}

void ne_set_notifier(ne_session *sess,
		     ne_notify_status status, void *userdata)
{
    sess->notify_cb = status;
    sess->notify_ud = userdata;
}

void ne_set_read_timeout(ne_session *sess, int timeout)
{
    sess->rdtimeout = timeout;
}

void ne_set_connect_timeout(ne_session *sess, int timeout)
{
    sess->cotimeout = timeout;
}

#define UAHDR "User-Agent: "
#define AGENT " neon/" NEON_VERSION "\r\n"

void ne_set_useragent(ne_session *sess, const char *token)
{
    if (sess->user_agent) ne_free(sess->user_agent);
    sess->user_agent = ne_concat(UAHDR, token, AGENT, NULL);
}

const char *ne_get_server_hostport(ne_session *sess)
{
    return sess->server.hostport;
}

const char *ne_get_scheme(ne_session *sess)
{
    return sess->scheme;
}

void ne_fill_server_uri(ne_session *sess, ne_uri *uri)
{
    uri->host = ne_strdup(sess->server.hostname);
    uri->port = sess->server.port;
    uri->scheme = ne_strdup(sess->scheme);
}

#ifdef WINSCP
void ne_set_realhost(ne_session *sess, const char *realhost)
{
    if (sess->realhost) ne_free(sess->realhost);
    sess->realhost = ne_strdup(realhost);
}
#endif

void ne_fill_proxy_uri(ne_session *sess, ne_uri *uri)
{
    if (sess->proxies) {
        struct host_info *hi = sess->nexthop ? sess->nexthop : sess->proxies;

        if (hi->proxy == PROXY_HTTP) {
            uri->host = ne_strdup(hi->hostname);
            uri->port = hi->port;
        }
    }
}

const char *ne_get_error(ne_session *sess)
{
    return sess->error;
}

void ne_close_connection(ne_session *sess)
{
    NE_DEBUG_WINSCP_CONTEXT(sess);
    if (sess->connected) {
        struct hook *hk;

        NE_DEBUG(NE_DBG_SOCKET, "sess: Closing connection.\n");

        if (sess->notify_cb) {
            sess->status.cd.hostname = sess->nexthop->hostname;
            sess->notify_cb(sess->notify_ud, ne_status_disconnected, 
                            &sess->status);
        }

        /* Run the close_conn hooks. */
        for (hk = sess->close_conn_hooks; hk != NULL; hk = hk->next) {
            ne_close_conn_fn fn = (ne_close_conn_fn)hk->fn;
            fn(hk->userdata);
        }

	ne_sock_close(sess->socket);
	sess->socket = NULL;
        NE_DEBUG(NE_DBG_SOCKET, "sess: Connection closed.\n");
    } else {
        NE_DEBUG(NE_DBG_SOCKET, "sess: Not closing closed connection.\n");
    }
    sess->connected = 0;
}

void ne_ssl_set_verify(ne_session *sess, ne_ssl_verify_fn fn, void *userdata)
{
    sess->ssl_verify_fn = fn;
    sess->ssl_verify_ud = userdata;
}

void ne_ssl_provide_clicert(ne_session *sess, 
			  ne_ssl_provide_fn fn, void *userdata)
{
    sess->ssl_provide_fn = fn;
    sess->ssl_provide_ud = userdata;
}

void ne_ssl_trust_cert(ne_session *sess, const ne_ssl_certificate *cert)
{
#ifdef NE_HAVE_SSL
    if (sess->ssl_context) {
        ne_ssl_context_trustcert(sess->ssl_context, cert);
    }
#endif
}

int ne_ssl_set_protovers(ne_session *sess, enum ne_ssl_protocol min,
                         enum ne_ssl_protocol max)
{
#ifdef NE_HAVE_SSL
    if (sess->ssl_context) {
        if (ne_ssl_context_set_versions(sess->ssl_context, min, max) != 0) {
            ne_set_error(sess, _("Could not set minimum/maximum SSL/TLS versions"));
            return NE_ERROR;
        }

        return NE_OK;
    }
#endif
    ne_set_error(sess, _("SSL/TLS not enabled for the session"));
    return NE_ERROR;
}

static const char *const ssl_proto_names[] = { "unknown", "SSLv3",
                                               "TLSv1.0", "TLSv1.1",
                                               "TLSv1.2", "TLSv1.3" };

const char *ne_ssl_proto_name(enum ne_ssl_protocol proto)
{
    if (proto < (sizeof(ssl_proto_names)/sizeof(ssl_proto_names[0])))
        return ssl_proto_names[proto];
    else
        return ssl_proto_names[0];
}

void ne_ssl_cert_validity(const ne_ssl_certificate *cert, char *from, char *until)
{
#ifdef NE_HAVE_SSL
    time_t tf, tu;
    char *date;

    ne_ssl_cert_validity_time(cert, &tf, &tu);
    
    if (from) {
        if (tf != (time_t) -1) {
            date = ne_rfc1123_date(tf);
            ne_strnzcpy(from, date, NE_SSL_VDATELEN);
            ne_free(date);
        }
        else {
            ne_strnzcpy(from, _("[invalid date]"), NE_SSL_VDATELEN);
        }
    }
        
    if (until) {
        if (tu != (time_t) -1) {
            date = ne_rfc1123_date(tu);
            ne_strnzcpy(until, date, NE_SSL_VDATELEN);
            ne_free(date);
        }
        else {
            ne_strnzcpy(until, _("[invalid date]"), NE_SSL_VDATELEN);
        }
    }
#endif
}

#ifdef NE_HAVE_SSL
void ne__ssl_set_verify_err(ne_session *sess, int failures)
{
    static const struct {
	int bit;
	const char *str;
    } reasons[] = {
	{ NE_SSL_NOTYETVALID, N_("certificate is not yet valid") },
	{ NE_SSL_EXPIRED, N_("certificate has expired") },
	{ NE_SSL_IDMISMATCH, N_("certificate issued for a different hostname") },
	{ NE_SSL_UNTRUSTED, N_("issuer is not trusted") },
        { NE_SSL_BADCHAIN, N_("bad certificate chain") },
        { NE_SSL_REVOKED, N_("certificate has been revoked") },
	{ 0, NULL }
    };
    int n, flag = 0;

    ne_strnzcpy(sess->error, _("Server certificate verification failed: "),
                sizeof sess->error);

    for (n = 0; reasons[n].bit; n++) {
	if (failures & reasons[n].bit) {
	    if (flag) strncat(sess->error, ", ", sizeof sess->error - 1);
	    strncat(sess->error, _(reasons[n].str), sizeof sess->error - 1);
	    flag = 1;
	}
    }
}

/* This doesn't actually implement complete RFC 2818 logic; omits
 * "f*.example.com" support for simplicity. */
int ne__ssl_match_hostname(const char *cn, size_t cnlen, const char *hostname)
{
    const char *dot;

    NE_DEBUG(NE_DBG_WINSCP_HTTP_DETAIL, "ssl: Match common name '%s' against '%s'\n",
             cn, hostname);

    if (strncmp(cn, "*.", 2) == 0 && cnlen > 2
        && (dot = strchr(hostname, '.')) != NULL) {
	hostname = dot + 1;
	cn += 2;
        cnlen -= 2;
    }

    return cnlen == strlen(hostname) && !ne_strcasecmp(cn, hostname);
}

#endif /* NE_HAVE_SSL */

typedef void (*void_fn)(void);

#define ADD_HOOK(hooks, fn, ud) add_hook(&(hooks), NULL, (void_fn)(fn), (ud))

static void add_hook(struct hook **hooks, const char *id, void_fn fn, void *ud)
{
    struct hook *hk = ne_malloc(sizeof (struct hook)), *pos;

    if (*hooks != NULL) {
	for (pos = *hooks; pos->next != NULL; pos = pos->next)
	    /* nullop */;
	pos->next = hk;
    } else {
	*hooks = hk;
    }

    hk->id = id;
    hk->fn = fn;
    hk->userdata = ud;
    hk->next = NULL;
}

void ne_hook_create_request(ne_session *sess, 
			    ne_create_request_fn fn, void *userdata)
{
    ADD_HOOK(sess->create_req_hooks, fn, userdata);
}

void ne_hook_pre_send(ne_session *sess, ne_pre_send_fn fn, void *userdata)
{
    ADD_HOOK(sess->pre_send_hooks, fn, userdata);
}

void ne_hook_post_send(ne_session *sess, ne_post_send_fn fn, void *userdata)
{
    ADD_HOOK(sess->post_send_hooks, fn, userdata);
}

void ne_hook_post_headers(ne_session *sess, ne_post_headers_fn fn, 
                          void *userdata)
{
    ADD_HOOK(sess->post_headers_hooks, fn, userdata);
}

void ne_hook_destroy_request(ne_session *sess,
			     ne_destroy_req_fn fn, void *userdata)
{
    ADD_HOOK(sess->destroy_req_hooks, fn, userdata);    
}

void ne_hook_destroy_session(ne_session *sess,
			     ne_destroy_sess_fn fn, void *userdata)
{
    ADD_HOOK(sess->destroy_sess_hooks, fn, userdata);
}

void ne_hook_close_conn(ne_session *sess,
                        ne_close_conn_fn fn, void *userdata)
{
    ADD_HOOK(sess->close_conn_hooks, fn, userdata);
}

void ne_set_session_private(ne_session *sess, const char *id, void *userdata)
{
    add_hook(&sess->private, id, NULL, userdata);
}

static void remove_hook(struct hook **hooks, void_fn fn, void *ud)
{
    struct hook **p = hooks;

    while (*p) {
        if ((*p)->fn == fn && (*p)->userdata == ud) {
            struct hook *next = (*p)->next;
            ne_free(*p);
            (*p) = next;
            break;
        }
        p = &(*p)->next;
    }
}

#define REMOVE_HOOK(hooks, fn, ud) remove_hook(&hooks, (void_fn)fn, ud)

void ne_unhook_create_request(ne_session *sess, 
                              ne_create_request_fn fn, void *userdata)
{
    REMOVE_HOOK(sess->create_req_hooks, fn, userdata);
}

void ne_unhook_pre_send(ne_session *sess, ne_pre_send_fn fn, void *userdata)
{
    REMOVE_HOOK(sess->pre_send_hooks, fn, userdata);
}

void ne_unhook_post_headers(ne_session *sess, ne_post_headers_fn fn, 
			    void *userdata)
{
    REMOVE_HOOK(sess->post_headers_hooks, fn, userdata);
}

void ne_unhook_post_send(ne_session *sess, ne_post_send_fn fn, void *userdata)
{
    REMOVE_HOOK(sess->post_send_hooks, fn, userdata);
}

void ne_unhook_destroy_request(ne_session *sess,
                               ne_destroy_req_fn fn, void *userdata)
{
    REMOVE_HOOK(sess->destroy_req_hooks, fn, userdata);    
}

void ne_unhook_destroy_session(ne_session *sess,
                               ne_destroy_sess_fn fn, void *userdata)
{
    REMOVE_HOOK(sess->destroy_sess_hooks, fn, userdata);
}

void ne_unhook_close_conn(ne_session *sess,
                          ne_close_conn_fn fn, void *userdata)
{
    REMOVE_HOOK(sess->close_conn_hooks, fn, userdata);
}
