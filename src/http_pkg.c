/*
 * Remote http:// and https:// package source.
 *
 * The console pulls byte ranges and feeds the existing stream installer.
 * On the PS5 this uses libSceHttp and libSceSsl, the same request API as
 * elfldr. Host tests keep a plain HTTP socket client.
 */

#include "http_pkg.h"
#include "multipart.h"
#include "installer.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define HTTP_PKG_MAX_REDIRECTS 5
#define HTTP_PKG_CONNECT_MS 10000
#define HTTP_PKG_IO_SEC 60
#define HTTP_PKG_HDR_MAX 8192
#define HTTP_PKG_READ_MAX (16U * 1024U * 1024U)

struct http_pkg_session {
    char url[HTTP_PKG_URL_MAX];
    char host[256];
    char host_header[300];
    char target[HTTP_PKG_URL_MAX];
    int port;
    int ipv6;
    int https;
    uint64_t size;
    int fd;
    int conn_id;
    pthread_mutex_t lock;
    int lock_ready;
};

static void set_err(char *err, size_t err_sz, const char *msg) {
    if (!err || err_sz == 0) return;
    snprintf(err, err_sz, "%s", msg ? msg : "");
}

#if defined(__Prospero__) || defined(PS5_BUILD)
/* libSceHttp / libSceSsl, matching elfldr uri.c. Method 0 is GET, version 2 is HTTP/1.1. */
#define SCE_HTTP_METHOD_GET 0
#define SCE_HTTP_VERSION_1_1 2
#define SCE_HTTP_HEADER_OVERWRITE 0

extern int sceNetInit(void);
extern int sceNetPoolCreate(const char *name, int size, int flags);
extern int sceSslInit(size_t poolSize);
extern int sceHttpInit(int memId, int sslId, size_t poolSize);
extern int sceHttpCreateTemplate(int httpCtxId, const char *userAgent, int httpVer, int proxy);
extern int sceHttpsSetSslCallback(int id, void *cb, void *userArg);
extern int sceHttpSetResponseHeaderMaxSize(int id, size_t size);
extern int sceHttpSetAutoRedirect(int id, int enable);
extern int sceHttpSetConnectTimeOut(int id, unsigned int usec);
extern int sceHttpSetResolveTimeOut(int id, unsigned int usec);
extern int sceHttpSetSendTimeOut(int id, unsigned int usec);
extern int sceHttpSetRecvTimeOut(int id, unsigned int usec);
extern int sceHttpCreateConnectionWithURL(int tmplId, const char *url, int keepalive);
extern int sceHttpDeleteConnection(int connId);
extern int sceHttpCreateRequestWithURL(int connId, int method, const char *url, uint64_t contentLength);
extern int sceHttpAddRequestHeader(int id, const char *name, const char *value, int mode);
extern int sceHttpSendRequest(int reqId, const void *postData, size_t size);
extern int sceHttpGetStatusCode(int reqId, int *statusCode);
extern int sceHttpGetAllResponseHeaders(int reqId, char **header, size_t *headerSize);
extern int sceHttpReadData(int reqId, void *data, unsigned int size);
extern int sceHttpDeleteRequest(int reqId);

static int g_http_pool = -1;
static int g_http_ssl = -1;
static int g_http_ctx = -1;
static int g_http_tmpl = -1;
static pthread_mutex_t g_http_lock = PTHREAD_MUTEX_INITIALIZER;

/* Return 0 to accept the certificate. elfldr does this so a link does not
 * need to be signed by a CA the console already trusts. */
static int http_ssl_cb(void) {
    return 0;
}

static int ps5_http_ensure(char *err, size_t err_sz) {
    if (g_http_tmpl >= 0) return 0;
    pthread_mutex_lock(&g_http_lock);
    if (g_http_tmpl < 0) {
        (void)sceNetInit();
        if (g_http_pool < 0) g_http_pool = sceNetPoolCreate("pkghttp", 256 * 1024, 0);
        if (g_http_pool >= 0 && g_http_ssl < 0) g_http_ssl = sceSslInit(512 * 1024);
        if (g_http_ssl >= 0 && g_http_ctx < 0) g_http_ctx = sceHttpInit(g_http_pool, g_http_ssl, 256 * 1024);
        if (g_http_ctx >= 0 && g_http_tmpl < 0) {
            int tmpl = sceHttpCreateTemplate(g_http_ctx, "pkgmgr", SCE_HTTP_VERSION_1_1, 1);
            if (tmpl >= 0 &&
                sceHttpSetResponseHeaderMaxSize(tmpl, 0x2000) >= 0 &&
                sceHttpsSetSslCallback(tmpl, (void *)http_ssl_cb, NULL) >= 0) {
                /* Follow redirects ourselves so later range reads use the final link. */
                if (sceHttpSetAutoRedirect(tmpl, 0) < 0) {
                    install_log("[HTTP] Could not disable automatic redirects");
                }
                (void)sceHttpSetConnectTimeOut(tmpl, 10U * 1000U * 1000U);
                (void)sceHttpSetResolveTimeOut(tmpl, 10U * 1000U * 1000U);
                (void)sceHttpSetSendTimeOut(tmpl, 60U * 1000U * 1000U);
                (void)sceHttpSetRecvTimeOut(tmpl, 60U * 1000U * 1000U);
                g_http_tmpl = tmpl;
            } else if (tmpl >= 0) {
                install_log("[HTTP] PS5 HTTP template setup failed tmpl=%d", tmpl);
            }
        }
    }
    pthread_mutex_unlock(&g_http_lock);
    if (g_http_tmpl < 0) {
        install_log("[HTTP] PS5 HTTP init failed pool=%d ssl=%d http=%d", g_http_pool, g_http_ssl, g_http_ctx);
        set_err(err, err_sz, "Could not connect to the server");
        return -1;
    }
    return 0;
}

static int ps5_open_conn(http_pkg_session_t *s, char *err, size_t err_sz) {
    int id;
    if (ps5_http_ensure(err, err_sz) != 0) return -1;
    if (s->conn_id >= 0) return 0;
    id = sceHttpCreateConnectionWithURL(g_http_tmpl, s->url, 1);
    if (id < 0) {
        install_log("[HTTP] sceHttpCreateConnectionWithURL 0x%08X %s", id, s->url);
        set_err(err, err_sz, "Could not connect to the server");
        return -1;
    }
    s->conn_id = id;
    return 0;
}
#endif

static int host_ok(const char *host, int ipv6) {
    size_t n = 0;
    if (!host || !host[0]) return 0;
    if (ipv6) {
        for (const char *p = host; *p; p++) {
            if (!isxdigit((unsigned char)*p) && *p != ':' && *p != '.') return 0;
            if (++n > 128) return 0;
        }
        return strchr(host, ':') != NULL;
    }
    if (host[0] == '.' || host[0] == '-') return 0;
    for (const char *p = host; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!isalnum(c) && c != '.' && c != '-' && c != '_') return 0;
        if (++n >= sizeof(((http_pkg_session_t *)0)->host)) return 0;
    }
    if (n == 0) return 0;
    return host[n - 1] != '.' && host[n - 1] != '-';
}

static int target_ok(const char *target) {
    if (!target || target[0] != '/') return 0;
    for (const char *p = target; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 32 || c == 127 || c == '\\' || c == '"' || c == '<' || c == '>') return 0;
    }
    return 1;
}

static int split_url(const char *url, char *host, size_t host_sz, int *port, int *ipv6, int *https,
                     char *target, size_t target_sz, char *err, size_t err_sz) {
    const char *p;
    int secure = 0;
    if (url && strncasecmp(url, "https://", 8) == 0) {
        secure = 1;
        p = url + 8;
    } else if (url && strncasecmp(url, "http://", 7) == 0) {
        p = url + 7;
    } else {
        set_err(err, err_sz, "Only http:// and https:// links are supported");
        return -1;
    }
    *https = secure;
    if (*p == '\0') {
        set_err(err, err_sz, "That link is not a valid http:// or https:// address");
        return -1;
    }
    {
        const char *slash = strchr(p, '/');
        const char *auth_end = slash ? slash : p + strlen(p);
        if (memchr(p, '@', (size_t)(auth_end - p))) {
            set_err(err, err_sz, "Links with a username or password are not supported");
            return -1;
        }
    }
    *ipv6 = 0;
    *port = secure ? 443 : 80;
    if (*p == '[') {
        const char *end = strchr(p, ']');
        size_t hlen;
        if (!end || end == p + 1) {
            set_err(err, err_sz, "That link is not a valid http:// or https:// address");
            return -1;
        }
        hlen = (size_t)(end - p - 1);
        if (hlen >= host_sz) {
            set_err(err, err_sz, "That link is not a valid http:// or https:// address");
            return -1;
        }
        memcpy(host, p + 1, hlen);
        host[hlen] = '\0';
        *ipv6 = 1;
        p = end + 1;
    } else {
        const char *end = p;
        size_t hlen;
        while (*end && *end != '/' && *end != ':' && *end != '?' && *end != '#') end++;
        hlen = (size_t)(end - p);
        if (hlen == 0 || hlen >= host_sz) {
            set_err(err, err_sz, "That link is not a valid http:// or https:// address");
            return -1;
        }
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        p = end;
    }
    if (*p == ':') {
        char *end = NULL;
        long value;
        p++;
        value = strtol(p, &end, 10);
        if (end == p || value <= 0 || value > 65535) {
            set_err(err, err_sz, "That link is not a valid http:// or https:// address");
            return -1;
        }
        *port = (int)value;
        p = end;
    }
    if (*p == '\0' || *p == '#') {
        snprintf(target, target_sz, "/");
    } else if (*p == '?') {
        snprintf(target, target_sz, "/%s", p);
    } else if (*p != '/') {
        set_err(err, err_sz, "That link is not a valid http:// or https:// address");
        return -1;
    } else if (strlen(p) >= target_sz) {
        set_err(err, err_sz, "That link is too long");
        return -1;
    } else {
        snprintf(target, target_sz, "%s", p);
    }
    {
        char *hash = strchr(target, '#');
        if (hash) *hash = '\0';
    }
    if (!host_ok(host, *ipv6) || !target_ok(target)) {
        set_err(err, err_sz, "That link is not a valid http:// or https:// address");
        return -1;
    }
    return 0;
}

static int build_canonical(const char *host, int port, int ipv6, int https, const char *target,
                           char *out, size_t out_sz, char *err, size_t err_sz) {
    const char *scheme = https ? "https" : "http";
    int defport = https ? 443 : 80;
    int n;
    if (ipv6) {
        n = port == defport ? snprintf(out, out_sz, "%s://[%s]%s", scheme, host, target)
                            : snprintf(out, out_sz, "%s://[%s]:%d%s", scheme, host, port, target);
    } else {
        n = port == defport ? snprintf(out, out_sz, "%s://%s%s", scheme, host, target)
                            : snprintf(out, out_sz, "%s://%s:%d%s", scheme, host, port, target);
    }
    if (n < 0 || (size_t)n >= out_sz || (size_t)n >= HTTP_PKG_URL_MAX) {
        set_err(err, err_sz, "That link is too long");
        return -1;
    }
    return 0;
}

int http_pkg_normalize(const char *url, char *out, size_t out_sz, char *err, size_t err_sz) {
    char trimmed[HTTP_PKG_URL_MAX + 8];
    char host[256], target[HTTP_PKG_URL_MAX];
    int port = 80, ipv6 = 0, https = 0;
    size_t len, begin = 0, end;
    if (!out || out_sz == 0) return -1;
    out[0] = '\0';
    if (!url || !url[0]) {
        set_err(err, err_sz, "Enter an http:// or https:// link");
        return -1;
    }
    while (url[begin] == ' ' || url[begin] == '\t') begin++;
    end = strlen(url + begin);
    while (end > 0 && (url[begin + end - 1] == ' ' || url[begin + end - 1] == '\t')) end--;
    if (end == 0) {
        set_err(err, err_sz, "Enter an http:// or https:// link");
        return -1;
    }
    if (end >= sizeof(trimmed)) {
        set_err(err, err_sz, "That link is too long");
        return -1;
    }
    memcpy(trimmed, url + begin, end);
    trimmed[end] = '\0';
    if (strncasecmp(trimmed, "https://", 8) != 0 && strncasecmp(trimmed, "http://", 7) != 0) {
        set_err(err, err_sz, "Only http:// and https:// links are supported");
        return -1;
    }
    len = strlen(trimmed);
    if (len >= HTTP_PKG_URL_MAX) {
        set_err(err, err_sz, "That link is too long");
        return -1;
    }
    if (split_url(trimmed, host, sizeof(host), &port, &ipv6, &https, target, sizeof(target), err, err_sz) != 0) {
        return -1;
    }
    return build_canonical(host, port, ipv6, https, target, out, out_sz, err, err_sz);
}

int http_pkg_is_url(const char *url) {
    char canon[HTTP_PKG_URL_MAX];
    return http_pkg_normalize(url, canon, sizeof(canon), NULL, 0) == 0;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void http_pkg_filename(const char *url, char *out, size_t out_sz) {
    const char *query, *start, *slash;
    char raw[256];
    size_t raw_len, o = 0;
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!url) {
        snprintf(out, out_sz, "package.pkg");
        return;
    }
    query = strchr(url, '?');
    start = strstr(url, "://");
    start = start ? start + 3 : url;
    slash = query ? query : url + strlen(url);
    while (slash > start && slash[-1] != '/') slash--;
    raw_len = (size_t)((query ? query : url + strlen(url)) - slash);
    if (raw_len >= sizeof(raw)) raw_len = sizeof(raw) - 1;
    memcpy(raw, slash, raw_len);
    raw[raw_len] = '\0';
    for (size_t i = 0; i < raw_len && o + 1 < out_sz; i++) {
        int hi, lo;
        if (raw[i] == '%' && i + 2 < raw_len &&
            (hi = hex_nibble(raw[i + 1])) >= 0 && (lo = hex_nibble(raw[i + 2])) >= 0) {
            int value = (hi << 4) | lo;
            if (value > 32 && value < 127 && value != '/' && value != '\\') out[o++] = (char)value;
            i += 2;
        } else if ((unsigned char)raw[i] > 32) {
            out[o++] = raw[i];
        }
    }
    out[o] = '\0';
    if (!out[0]) snprintf(out, out_sz, "package.pkg");
}

static int apply_parts(http_pkg_session_t *s, const char *url, char *err, size_t err_sz) {
    char canon[HTTP_PKG_URL_MAX];
    if (http_pkg_normalize(url, canon, sizeof(canon), err, err_sz) != 0) return -1;
    if (split_url(canon, s->host, sizeof(s->host), &s->port, &s->ipv6, &s->https,
                  s->target, sizeof(s->target), err, err_sz) != 0) return -1;
    {
        int defport = s->https ? 443 : 80;
        if (s->ipv6) {
            if (s->port == defport) snprintf(s->host_header, sizeof(s->host_header), "[%s]", s->host);
            else snprintf(s->host_header, sizeof(s->host_header), "[%s]:%d", s->host, s->port);
        } else if (s->port == defport) {
            snprintf(s->host_header, sizeof(s->host_header), "%s", s->host);
        } else {
            snprintf(s->host_header, sizeof(s->host_header), "%s:%d", s->host, s->port);
        }
    }
    snprintf(s->url, sizeof(s->url), "%s", canon);
    return 0;
}

static void close_fd(http_pkg_session_t *s) {
#if defined(__Prospero__) || defined(PS5_BUILD)
    if (s && s->conn_id >= 0) {
        sceHttpDeleteConnection(s->conn_id);
        s->conn_id = -1;
    }
#else
    if (s && s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
#endif
}

#if !defined(__Prospero__) && !defined(PS5_BUILD)
static int connect_host(http_pkg_session_t *s, char *err, size_t err_sz) {
    char port[8];
    struct addrinfo hints, *res = NULL, *ai;
    int fd = -1;
    snprintf(port, sizeof(port), "%d", s->port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(s->host, port, &hints, &res) != 0) {
        set_err(err, err_sz, "Could not resolve the host");
        return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        int flags, rc, soerr = 0;
        socklen_t sl = sizeof(soerr);
        struct pollfd pfd;
        struct timeval tv;
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
#ifdef SO_NOSIGPIPE
        {
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        }
#endif
        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            fd = -1;
            continue;
        }
        rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        do {
            rc = poll(&pfd, 1, HTTP_PKG_CONNECT_MS);
        } while (rc < 0 && errno == EINTR);
        if (rc <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 || soerr != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        fcntl(fd, F_SETFL, flags);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        tv.tv_sec = HTTP_PKG_IO_SEC;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef TCP_NODELAY
        {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        }
#endif
        break;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        set_err(err, err_sz, "Could not connect to the server");
        return -1;
    }
    close_fd(s);
    s->fd = fd;
    return 0;
}

static int send_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static int recv_headers(int fd, char *buf, size_t cap, size_t *hdr_len, size_t *total) {
    size_t n = 0, scanned = 0;
    while (n + 1 < cap) {
        ssize_t r = recv(fd, buf + n, cap - 1 - n, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;
        n += (size_t)r;
        /* Scan after every read: one recv can fill the buffer with the
         * headers plus the start of the body. */
        for (size_t i = scanned; i + 4 <= n; i++) {
            if (memcmp(buf + i, "\r\n\r\n", 4) == 0) {
                *hdr_len = i + 4;
                *total = n;
                buf[n] = '\0';
                return 0;
            }
        }
        scanned = n >= 3 ? n - 3 : 0;
    }
    return -1;
}

#endif /* host socket client */

static const char *header_value(const char *headers, const char *name) {
    size_t nlen = strlen(name);
    const char *p = headers;
    while (p && *p) {
        if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            p += nlen + 1;
            while (*p == ' ' || *p == '\t') p++;
            return p;
        }
        const char *nl = strstr(p, "\r\n");
        if (!nl) return NULL;
        p = nl + 2;
    }
    return NULL;
}

static int header_u64(const char *headers, const char *name, uint64_t *out) {
    const char *v = header_value(headers, name);
    char *end = NULL;
    unsigned long long n;
    if (!v) return -1;
    n = strtoull(v, &end, 10);
    if (end == v) return -1;
    *out = (uint64_t)n;
    return 0;
}

static int parse_content_range(const char *value, uint64_t *start, uint64_t *end, uint64_t *total) {
    unsigned long long s, e, t;
    if (!value) return -1;
    while (*value == ' ' || *value == '\t') value++;
    if (strncasecmp(value, "bytes", 5) != 0) return -1;
    value += 5;
    while (*value == ' ' || *value == '\t' || *value == '=') value++;
    if (sscanf(value, "%llu-%llu/%llu", &s, &e, &t) != 3) return -1;
    if (e < s || t == 0 || e >= t) return -1;
    *start = (uint64_t)s;
    *end = (uint64_t)e;
    *total = (uint64_t)t;
    return 0;
}

#if !defined(__Prospero__) && !defined(PS5_BUILD)
static int read_exact(int fd, uint8_t *dst, size_t len, const uint8_t *prefix, size_t prefix_len) {
    size_t off = 0;
    if (prefix_len > len) return -1;
    if (prefix_len) {
        memcpy(dst, prefix, prefix_len);
        off = prefix_len;
    }
    while (off < len) {
        ssize_t n = recv(fd, dst + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}
#endif /* host socket reads */

static int format_http_url(char *dest, size_t dest_sz, const http_pkg_session_t *s, const char *path) {
    const char *scheme = s->https ? "https" : "http";
    int defport = s->https ? 443 : 80;
    int n;
    if (!path || path[0] != '/') return -1;
    if (s->ipv6) {
        n = s->port == defport ? snprintf(dest, dest_sz, "%s://[%s]%s", scheme, s->host, path)
                               : snprintf(dest, dest_sz, "%s://[%s]:%d%s", scheme, s->host, s->port, path);
    } else if (s->port == defport) {
        n = snprintf(dest, dest_sz, "%s://%s%s", scheme, s->host, path);
    } else {
        n = snprintf(dest, dest_sz, "%s://%s:%d%s", scheme, s->host, s->port, path);
    }
    return (n < 0 || (size_t)n >= dest_sz) ? -1 : 0;
}

static int resolve_redirect(http_pkg_session_t *s, const char *location, char *dest, size_t dest_sz,
                            char *err, size_t err_sz) {
    char next[HTTP_PKG_URL_MAX];
    char path[HTTP_PKG_URL_MAX];
    while (location && (*location == ' ' || *location == '\t')) location++;
    if (!location || !location[0]) {
        set_err(err, err_sz, "Could not read the remote package");
        return -1;
    }
    if (strncasecmp(location, "http://", 7) == 0 || strncasecmp(location, "https://", 8) == 0 ||
        strncmp(location, "//", 2) == 0) {
        int n = strncmp(location, "//", 2) == 0
            ? snprintf(next, sizeof(next), "%s:%s", s->https ? "https" : "http", location)
            : snprintf(next, sizeof(next), "%s", location);
        if (n < 0 || (size_t)n >= sizeof(next)) {
            set_err(err, err_sz, "That link is too long");
            return -1;
        }
    } else {
        int n;
        if (location[0] == '/') {
            n = snprintf(path, sizeof(path), "%s", location);
        } else if (location[0] == '?') {
            char base[HTTP_PKG_URL_MAX];
            char *query;
            snprintf(base, sizeof(base), "%s", s->target);
            query = strchr(base, '?');
            if (query) *query = '\0';
            n = snprintf(path, sizeof(path), "%s%s", base, location);
        } else {
            char base[HTTP_PKG_URL_MAX];
            char *slash;
            snprintf(base, sizeof(base), "%s", s->target);
            slash = strrchr(base, '/');
            if (!slash) {
                snprintf(base, sizeof(base), "/");
            } else {
                char *query = strchr(slash, '?');
                if (query) *query = '\0';
                slash[1] = '\0';
            }
            n = snprintf(path, sizeof(path), "%s%s", base, location);
        }
        if (n < 0 || (size_t)n >= sizeof(path) || format_http_url(next, sizeof(next), s, path) != 0) {
            set_err(err, err_sz, "That link is too long");
            return -1;
        }
    }
    {
        char *cr = strpbrk(next, "\r\n");
        if (cr) *cr = '\0';
    }
    if (http_pkg_normalize(next, dest, dest_sz, err, err_sz) != 0) {
        if (err && err[0] == '\0') set_err(err, err_sz, "Redirected link is not a valid http:// or https:// address");
        return -1;
    }
    return 0;
}

#if defined(__Prospero__) || defined(PS5_BUILD)
static int fetch_range(http_pkg_session_t *s, uint64_t start, uint64_t end, uint8_t *body, size_t body_len,
                       char *err, size_t err_sz) {
    char location_url[HTTP_PKG_URL_MAX];
    for (int hop = 0; hop <= HTTP_PKG_MAX_REDIRECTS; hop++) {
        char range[80];
        char hdrbuf[HTTP_PKG_HDR_MAX];
        int req = -1;
        int status = 0;
        int sent = 0;
        uint64_t range_start = 0, range_end = 0, range_total = 0;
        size_t got = 0;
        hdrbuf[0] = '\0';
        snprintf(range, sizeof(range), "bytes=%llu-%llu",
                 (unsigned long long)start, (unsigned long long)end);
        for (int attempt = 0; attempt < 2 && !sent; attempt++) {
            char *hdr = NULL;
            size_t hdr_len = 0;
            int rc;
            if (ps5_open_conn(s, err, err_sz) != 0) return -1;
            req = sceHttpCreateRequestWithURL(s->conn_id, SCE_HTTP_METHOD_GET, s->url, 0);
            if (req < 0) {
                close_fd(s);
                if (attempt == 0) continue;
                set_err(err, err_sz, "Could not read the remote package");
                return -1;
            }
            if (sceHttpAddRequestHeader(req, "Range", range, SCE_HTTP_HEADER_OVERWRITE) < 0 ||
                sceHttpAddRequestHeader(req, "Accept-Encoding", "identity", SCE_HTTP_HEADER_OVERWRITE) < 0) {
                sceHttpDeleteRequest(req);
                set_err(err, err_sz, "Could not read the remote package");
                return -1;
            }
            rc = sceHttpSendRequest(req, NULL, 0);
            if (rc < 0) {
                install_log("[HTTP] sceHttpSendRequest 0x%08X %s", rc, s->url);
                sceHttpDeleteRequest(req);
                close_fd(s);
                req = -1;
                if (attempt == 0) continue;
                set_err(err, err_sz, "Could not read the remote package");
                return -1;
            }
            if (sceHttpGetStatusCode(req, &status) < 0) {
                sceHttpDeleteRequest(req);
                close_fd(s);
                set_err(err, err_sz, "Could not read the remote package");
                return -1;
            }
            if (sceHttpGetAllResponseHeaders(req, &hdr, &hdr_len) >= 0 && hdr && hdr_len > 0) {
                size_t n = hdr_len < sizeof(hdrbuf) - 1 ? hdr_len : sizeof(hdrbuf) - 1;
                memcpy(hdrbuf, hdr, n);
                hdrbuf[n] = '\0';
            }
            sent = 1;
        }
        if (!sent || req < 0) {
            set_err(err, err_sz, "Could not read the remote package");
            return -1;
        }
        if (status >= 300 && status < 400) {
            char loc[HTTP_PKG_URL_MAX];
            const char *raw = header_value(hdrbuf, "Location");
            size_t loc_len;
            sceHttpDeleteRequest(req);
            close_fd(s);
            if (!raw) {
                set_err(err, err_sz, "Could not read the remote package");
                return -1;
            }
            while (*raw == ' ' || *raw == '\t') raw++;
            loc_len = strcspn(raw, "\r\n");
            while (loc_len > 0 && (raw[loc_len - 1] == ' ' || raw[loc_len - 1] == '\t')) loc_len--;
            if (loc_len == 0 || loc_len >= sizeof(loc)) {
                set_err(err, err_sz, loc_len ? "That link is too long" : "Could not read the remote package");
                return -1;
            }
            memcpy(loc, raw, loc_len);
            loc[loc_len] = '\0';
            if (resolve_redirect(s, loc, location_url, sizeof(location_url), err, err_sz) != 0) return -1;
            if (apply_parts(s, location_url, err, err_sz) != 0) return -1;
            continue;
        }
        if (status != 206) {
            sceHttpDeleteRequest(req);
            close_fd(s);
            set_err(err, err_sz, status == 200 ? "The server does not support range downloads"
                                               : "Could not read the remote package");
            return -1;
        }
        if (parse_content_range(header_value(hdrbuf, "Content-Range"), &range_start, &range_end, &range_total) != 0 ||
            range_start != start || range_end != end || range_end < range_start ||
            range_end - range_start + 1 != body_len ||
            (s->size != 0 && s->size != range_total)) {
            sceHttpDeleteRequest(req);
            close_fd(s);
            set_err(err, err_sz, "Could not read the remote package");
            return -1;
        }
        s->size = range_total;
        while (got < body_len) {
            unsigned int chunk = (unsigned int)(body_len - got > 0x100000u ? 0x100000u : body_len - got);
            int n = sceHttpReadData(req, body + got, chunk);
            if (n <= 0) {
                sceHttpDeleteRequest(req);
                close_fd(s);
                set_err(err, err_sz, "Could not read the remote package");
                return -1;
            }
            got += (size_t)n;
        }
        sceHttpDeleteRequest(req);
        return 0;
    }
    close_fd(s);
    set_err(err, err_sz, "Too many redirects");
    return -1;
}
#else
static int fetch_range(http_pkg_session_t *s, uint64_t start, uint64_t end, uint8_t *body, size_t body_len,
                       char *err, size_t err_sz) {
    char location_url[HTTP_PKG_URL_MAX];
    for (int hop = 0; hop <= HTTP_PKG_MAX_REDIRECTS; hop++) {
        char req[HTTP_PKG_URL_MAX + 512];
        char hdr[HTTP_PKG_HDR_MAX];
        size_t hdr_len = 0, total = 0;
        int status = 0, keep = 1;
        uint64_t content_len = 0, range_start = 0, range_end = 0, range_total = 0;
        const char *encoding;
        const uint8_t *prefix;
        size_t prefix_len;
        int n;
        if (s->https) {
            close_fd(s);
            set_err(err, err_sz, "https:// links are fetched by the PS5");
            return -1;
        }
        n = snprintf(req, sizeof(req),
                         "GET %s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "User-Agent: pkgmgr\r\n"
                         "Range: bytes=%llu-%llu\r\n"
                         "Accept: */*\r\n"
                         "Connection: keep-alive\r\n"
                         "\r\n",
                         s->target, s->host_header,
                         (unsigned long long)start, (unsigned long long)end);
        if (n < 0 || (size_t)n >= sizeof(req)) {
            set_err(err, err_sz, "That link is too long");
            return -1;
        }
        if (s->fd < 0 && connect_host(s, err, err_sz) != 0) return -1;
        if (send_all(s->fd, req, (size_t)n) != 0 || recv_headers(s->fd, hdr, sizeof(hdr), &hdr_len, &total) != 0) {
            close_fd(s);
            if (hop == HTTP_PKG_MAX_REDIRECTS) {
                set_err(err, err_sz, "Could not read the remote package");
                return -1;
            }
            if (connect_host(s, err, err_sz) != 0) return -1;
            if (send_all(s->fd, req, (size_t)n) != 0 || recv_headers(s->fd, hdr, sizeof(hdr), &hdr_len, &total) != 0) {
                close_fd(s);
                set_err(err, err_sz, "Could not read the remote package");
                return -1;
            }
        }
        if (sscanf(hdr, "HTTP/%*d.%*d %d", &status) != 1) {
            close_fd(s);
            set_err(err, err_sz, "Could not read the remote package");
            return -1;
        }
        keep = strncmp(hdr, "HTTP/1.0", 8) != 0;
        {
            const char *conn = header_value(hdr, "Connection");
            if (conn && strncasecmp(conn, "close", 5) == 0) keep = 0;
            else if (conn && strncasecmp(conn, "keep-alive", 10) == 0) keep = 1;
        }
        prefix = (const uint8_t *)hdr + hdr_len;
        prefix_len = total - hdr_len;
        encoding = header_value(hdr, "Transfer-Encoding");
        if (encoding) {
            const char *e = encoding;
            while (*e == ' ' || *e == '\t') e++;
            if (strncasecmp(e, "identity", 8) != 0) {
                close_fd(s);
                set_err(err, err_sz, "Could not read the remote package");
                return -1;
            }
            e += 8;
            while (*e == ' ' || *e == '\t') e++;
            if (*e != '\0' && *e != '\r') {
                close_fd(s);
                set_err(err, err_sz, "Could not read the remote package");
                return -1;
            }
        }
        if (status >= 300 && status < 400) {
            char loc[HTTP_PKG_URL_MAX];
            const char *raw = header_value(hdr, "Location");
            size_t loc_len;
            /* Drop the socket. Redirect bodies are not needed, and reading
             * them is what would keep a hostile response from stalling us. */
            close_fd(s);
            if (!raw) {
                set_err(err, err_sz, "Could not read the remote package");
                return -1;
            }
            while (*raw == ' ' || *raw == '\t') raw++;
            loc_len = strcspn(raw, "\r\n");
            while (loc_len > 0 && (raw[loc_len - 1] == ' ' || raw[loc_len - 1] == '\t')) loc_len--;
            if (loc_len == 0 || loc_len >= sizeof(loc)) {
                set_err(err, err_sz, loc_len ? "That link is too long" : "Could not read the remote package");
                return -1;
            }
            memcpy(loc, raw, loc_len);
            loc[loc_len] = '\0';
            if (resolve_redirect(s, loc, location_url, sizeof(location_url), err, err_sz) != 0) return -1;
            if (apply_parts(s, location_url, err, err_sz) != 0) return -1;
            continue;
        }
        if (status != 206) {
            close_fd(s);
            if (status == 200) set_err(err, err_sz, "The server does not support range downloads");
            else if (status == 404) set_err(err, err_sz, "Could not read the remote package");
            else set_err(err, err_sz, "Could not read the remote package");
            return -1;
        }
        if (parse_content_range(header_value(hdr, "Content-Range"), &range_start, &range_end, &range_total) != 0 ||
            range_start != start || range_end != end || range_end < range_start ||
            range_end - range_start + 1 != body_len) {
            close_fd(s);
            set_err(err, err_sz, "Could not read the remote package");
            return -1;
        }
        if (s->size != 0 && s->size != range_total) {
            close_fd(s);
            set_err(err, err_sz, "Could not read the remote package");
            return -1;
        }
        s->size = range_total;
        if (header_u64(hdr, "Content-Length", &content_len) != 0) {
            /* A keep-alive response without a length would leave unread bytes
             * in front of the next range request. */
            keep = 0;
        } else if (content_len != body_len) {
            close_fd(s);
            set_err(err, err_sz, "Could not read the remote package");
            return -1;
        }
        if (read_exact(s->fd, body, body_len, prefix, prefix_len) != 0) {
            close_fd(s);
            set_err(err, err_sz, "Could not read the remote package");
            return -1;
        }
        if (!keep) close_fd(s);
        return 0;
    }
    close_fd(s);
    set_err(err, err_sz, "Too many redirects");
    return -1;
}
#endif

int http_pkg_session_open(const char *url, http_pkg_session_t **out, char *err, size_t err_sz) {
    http_pkg_session_t *s;
    uint8_t probe;
    if (out) *out = NULL;
    if (!out) return -1;
    s = calloc(1, sizeof(*s));
    if (!s) {
        set_err(err, err_sz, "Could not read the remote package");
        return -1;
    }
    s->fd = -1;
    s->conn_id = -1;
    if (apply_parts(s, url, err, err_sz) != 0) {
        free(s);
        return -1;
    }
    if (fetch_range(s, 0, 0, &probe, 1, err, err_sz) != 0 || s->size == 0) {
        if (err && err[0] == '\0') set_err(err, err_sz, "The remote file is empty");
        close_fd(s);
        free(s);
        return -1;
    }
    if (pthread_mutex_init(&s->lock, NULL) != 0) {
        close_fd(s);
        free(s);
        set_err(err, err_sz, "Could not read the remote package");
        return -1;
    }
    s->lock_ready = 1;
    *out = s;
    return 0;
}

ssize_t http_pkg_session_read(http_pkg_session_t *session, void *buf, size_t count, uint64_t offset) {
    uint64_t end;
    char err[160];
    if (!session || !buf) return -1;
    pthread_mutex_lock(&session->lock);
    if (offset >= session->size) {
        pthread_mutex_unlock(&session->lock);
        return 0;
    }
    if (count > session->size - offset) count = (size_t)(session->size - offset);
    if (count == 0 || count > HTTP_PKG_READ_MAX || offset > UINT64_MAX - (count - 1)) {
        pthread_mutex_unlock(&session->lock);
        return count == 0 ? 0 : -1;
    }
    end = offset + count - 1;
    if (fetch_range(session, offset, end, (uint8_t *)buf, count, err, sizeof(err)) != 0) {
        pthread_mutex_unlock(&session->lock);
        return -1;
    }
    pthread_mutex_unlock(&session->lock);
    return (ssize_t)count;
}

uint64_t http_pkg_session_size(const http_pkg_session_t *session) {
    return session ? session->size : 0;
}

void http_pkg_session_close(http_pkg_session_t *session) {
    if (!session) return;
    if (session->lock_ready) pthread_mutex_lock(&session->lock);
    close_fd(session);
    if (session->lock_ready) {
        pthread_mutex_unlock(&session->lock);
        pthread_mutex_destroy(&session->lock);
    }
    free(session);
}

ssize_t http_pkg_pread(const char *url, void *buf, size_t count, uint64_t offset) {
    http_pkg_session_t *session = NULL;
    ssize_t n;
    if (http_pkg_session_open(url, &session, NULL, 0) != 0) return -1;
    n = http_pkg_session_read(session, buf, count, offset);
    http_pkg_session_close(session);
    return n;
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t read_le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i]) << (i * 8);
    return v;
}

static int sdk_http_read(void *ctx, uint64_t offset, void *buf, size_t size) {
    return http_pkg_session_read((http_pkg_session_t *)ctx, buf, size, offset) == (ssize_t)size ? 0 : -1;
}

static int parse_session(http_pkg_session_t *sess, const char *url, pkg_detail_t *out) {
    uint8_t hdr[0x200];
    ssize_t hdr_read;
    uint64_t cnt_offset = 0;
    int cnt_found = 0, is_lih;
    uint8_t cnt_hdr[0x80];
    uint32_t cnt_type_magic, entry_count, table_offset, content_flags;
    size_t table_size;
    uint8_t *entry_table = NULL;
    char *str_table = NULL;
    uint32_t str_table_off = 0, str_table_sz = 0;
    int has_playgo_chunk_patch = 0, has_delta_patch = 0, has_base_app_metadata = 0, has_ps4_sfo_category = 0;
    int is_delta_type;

    memset(out, 0, sizeof(*out));
    snprintf(out->path, sizeof(out->path), "%s", url);
    http_pkg_filename(url, out->filename, sizeof(out->filename));
    out->file_size = http_pkg_session_size(sess);
    out->total_pkg_size = out->file_size;

    hdr_read = http_pkg_session_read(sess, hdr, sizeof(hdr), 0);
    if (hdr_read < 0x80) return -1;
    if (hdr_read >= MULTIPART_MAGIC_LEN && memcmp(hdr, MULTIPART_MAGIC, MULTIPART_MAGIC_LEN) == 0) return -1;

    is_lih = memcmp(hdr, "\x7f" "LIH", 4) == 0;
    if (is_lih) {
        uint64_t cand = read_le64(hdr + 0x30);
        uint8_t magic[4];
        if (hdr[6] == 1 && hdr[7] == 0 && cand >= 0x80 &&
            cand <= out->file_size && 0x80 <= out->file_size - cand &&
            http_pkg_session_read(sess, magic, sizeof(magic), cand) == (ssize_t)sizeof(magic) &&
            memcmp(magic, "\x7f" "CNT", 4) == 0) {
            cnt_offset = cand;
            cnt_found = 1;
        }
    } else if (memcmp(hdr, "\x7f" "CNT", 4) == 0) {
        cnt_offset = 0;
        cnt_found = 1;
    } else if (memcmp(hdr, "\x7f" "FIH", 4) == 0) {
        uint64_t cand = read_le64(hdr + 0x58);
        if (cand > 0 && cand < out->file_size) {
            uint8_t test_magic[4];
            if (http_pkg_session_read(sess, test_magic, 4, cand) == 4 && memcmp(test_magic, "\x7f" "CNT", 4) == 0) {
                cnt_offset = cand;
                cnt_found = 1;
            }
        }
        if (!cnt_found) {
            for (size_t off = 0x10; off + 8 <= (size_t)hdr_read; off += 0x08) {
                cand = read_le64(hdr + off);
                if (cand >= 0x10000 && cand < out->file_size && (cand % 0x1000) == 0) {
                    uint8_t test_magic[4];
                    if (http_pkg_session_read(sess, test_magic, 4, cand) == 4 && memcmp(test_magic, "\x7f" "CNT", 4) == 0) {
                        cnt_offset = cand;
                        cnt_found = 1;
                        break;
                    }
                }
            }
        }
    }
    if (!cnt_found) return -1;
    if (http_pkg_session_read(sess, cnt_hdr, sizeof(cnt_hdr), cnt_offset) != (ssize_t)sizeof(cnt_hdr) ||
        memcmp(cnt_hdr, "\x7f" "CNT", 4) != 0) return -1;

    cnt_type_magic = read_be32(cnt_hdr + 0x04);
    memcpy(out->content_id, cnt_hdr + 0x40, 48);
    out->content_id[48] = '\0';
    for (int i = 0; i < 48; i++) {
        if ((unsigned char)out->content_id[i] < 32 || (unsigned char)out->content_id[i] > 126) {
            out->content_id[i] = '\0';
            break;
        }
    }
    entry_count = read_be32(cnt_hdr + 0x10);
    table_offset = read_be32(cnt_hdr + 0x18);
    if (entry_count == 0 || entry_count > 2048 || table_offset > 0x200000) return -1;
    table_size = (size_t)entry_count * 32;
    entry_table = malloc(table_size);
    if (!entry_table) return -1;
    if (http_pkg_session_read(sess, entry_table, table_size, cnt_offset + table_offset) != (ssize_t)table_size) {
        free(entry_table);
        return -1;
    }
    for (uint32_t i = 0; i < entry_count; i++) {
        const uint8_t *e = entry_table + i * 32;
        if (read_be32(e) == 0x0200) {
            str_table_off = read_be32(e + 16);
            str_table_sz = read_be32(e + 20);
            break;
        }
    }
    if (str_table_sz > 0 && str_table_sz < 65536) {
        str_table = malloc((size_t)str_table_sz + 1);
        if (str_table) {
            if (http_pkg_session_read(sess, str_table, str_table_sz, cnt_offset + str_table_off) == (ssize_t)str_table_sz) {
                str_table[str_table_sz] = '\0';
            } else {
                free(str_table);
                str_table = NULL;
            }
        }
    }
    for (uint32_t i = 0; i < entry_count; i++) {
        const uint8_t *e = entry_table + i * 32;
        uint32_t type = read_be32(e);
        uint32_t fn_off = read_be32(e + 4);
        uint32_t data_off = read_be32(e + 16);
        uint32_t data_sz = read_be32(e + 20);
        const char *name = "";
        if (str_table && fn_off < str_table_sz) name = str_table + fn_off;
        if (type == 0x1008 || strcmp(name, "app/playgo-chunk.dat") == 0) has_playgo_chunk_patch = 1;
        if (type == 0x0407 || type == 0x0408 ||
            strcmp(name, "target-deltainfo.dat") == 0 || strcmp(name, "origin-deltainfo.dat") == 0) {
            has_delta_patch = 1;
        }
        if ((type == 0x2000 || strcmp(name, "param.json") == 0) && data_sz > 0 && data_sz < 262144) {
            char *json_buf = malloc((size_t)data_sz + 1);
            if (json_buf) {
                if (http_pkg_session_read(sess, json_buf, data_sz, cnt_offset + data_off) == (ssize_t)data_sz) {
                    json_buf[data_sz] = '\0';
                    if (strstr(json_buf, "\"applicationDrmType\"") ||
                        strstr(json_buf, "\"applicationCategoryType\"") ||
                        strstr(json_buf, "\"contentBadgeType\"")) {
                        has_base_app_metadata = 1;
                    }
                    pkg_parser_parse_param_json(json_buf, data_sz,
                                                out->title_id, sizeof(out->title_id),
                                                out->title_name, sizeof(out->title_name),
                                                out->category, sizeof(out->category),
                                                out->app_version, sizeof(out->app_version),
                                                out->localized_titles, sizeof(out->localized_titles),
                                                out->default_language, sizeof(out->default_language));
                }
                free(json_buf);
            }
        }
        if ((type == 0x1000 || strcmp(name, "param.sfo") == 0) && data_sz > 0 && data_sz < 262144) {
            uint8_t *sfo_buf = malloc(data_sz);
            if (sfo_buf) {
                if (http_pkg_session_read(sess, sfo_buf, data_sz, cnt_offset + data_off) == (ssize_t)data_sz) {
                    char stitle[PKG_TITLE_NAME_LEN] = {0};
                    char stid[PKG_TITLE_ID_LEN] = {0};
                    char sver[32] = {0};
                    char sfo_category[sizeof(out->category)] = {0};
                    pkg_parser_parse_param_sfo(sfo_buf, data_sz, stitle, sizeof(stitle),
                                               stid, sizeof(stid), sver, sizeof(sver),
                                               sfo_category, sizeof(sfo_category),
                                               out->localized_titles, sizeof(out->localized_titles),
                                               out->default_language, sizeof(out->default_language));
                    if (sfo_category[0] != '\0' && out->category[0] == '\0') {
                        has_ps4_sfo_category = 1;
                        snprintf(out->category, sizeof(out->category), "%s", sfo_category);
                    }
                    if (out->title_id[0] == '\0' && stid[0] != '\0')
                        snprintf(out->title_id, sizeof(out->title_id), "%s", stid);
                    if (out->title_name[0] == '\0' && stitle[0] != '\0')
                        snprintf(out->title_name, sizeof(out->title_name), "%s", stitle);
                    if (out->app_version[0] == '\0' && sver[0] != '\0')
                        snprintf(out->app_version, sizeof(out->app_version), "%s", sver);
                }
                free(sfo_buf);
            }
        }
        if ((type == 0x1200 || strcmp(name, "icon0.png") == 0) && data_sz > 0) {
            out->has_icon = 1;
            out->icon_offset = cnt_offset + data_off;
            out->icon_size = data_sz;
        }
    }
    is_delta_type = ((cnt_type_magic & 0xFF) == 0x1E || (cnt_type_magic & 0xFF000000) == 0x41000000);
    content_flags = read_be32(cnt_hdr + 0x78);
    if (is_lih || (content_flags & 0x40100000) || has_playgo_chunk_patch || has_delta_patch || is_delta_type ||
        (out->category[0] != '\0' && strncmp(out->category, "gp", 2) == 0)) {
        out->pkg_type = PKG_TYPE_UPDATE;
    } else if (strncmp(out->category, "ac", 2) == 0 || strncmp(out->category, "al", 2) == 0 ||
               strcmp(out->category, "addcont") == 0) {
        out->pkg_type = PKG_TYPE_DLC;
    } else if (has_ps4_sfo_category &&
               (strncmp(out->category, "gd", 2) == 0 || strncmp(out->category, "bd", 2) == 0 ||
                strncmp(out->category, "gc", 2) == 0 || strncmp(out->category, "wt", 2) == 0)) {
        out->pkg_type = PKG_TYPE_BASE;
    } else if ((cnt_type_magic & 0xFF) == 1 && !has_base_app_metadata) {
        out->pkg_type = PKG_TYPE_DLC;
    } else if (strncmp(out->category, "gd", 2) == 0 || strncmp(out->category, "bd", 2) == 0 ||
               strncmp(out->category, "gc", 2) == 0 || strncmp(out->category, "wt", 2) == 0) {
        out->pkg_type = PKG_TYPE_BASE;
    }
    if (out->pkg_type == PKG_TYPE_UNKNOWN) out->pkg_type = PKG_TYPE_BASE;
    switch (out->pkg_type) {
        case PKG_TYPE_BASE: snprintf(out->pkg_type_str, sizeof(out->pkg_type_str), "base"); break;
        case PKG_TYPE_UPDATE: snprintf(out->pkg_type_str, sizeof(out->pkg_type_str), "update"); break;
        case PKG_TYPE_DLC: snprintf(out->pkg_type_str, sizeof(out->pkg_type_str), "dlc"); break;
        default: snprintf(out->pkg_type_str, sizeof(out->pkg_type_str), "unknown"); break;
    }
    free(str_table);
    free(entry_table);
    if (out->title_id[0] == '\0' && out->content_id[0] != '\0') {
        const char *dash = strchr(out->content_id, '-');
        if (dash) {
            const char *us = strchr(dash + 1, '_');
            if (us && (size_t)(us - (dash + 1)) < sizeof(out->title_id)) {
                size_t n = (size_t)(us - (dash + 1));
                memcpy(out->title_id, dash + 1, n);
                out->title_id[n] = '\0';
            }
        }
    }
    if (out->title_name[0] == '\0') {
        snprintf(out->title_name, sizeof(out->title_name), "%s", out->title_id[0] ? out->title_id : "Unknown Package");
    }
    if (strncmp(out->title_id, "PPSA", 4) == 0) {
        out->sdk_version = pkg_parser_read_image_sdk(sdk_http_read, sess, hdr, cnt_offset);
    }
    out->is_valid = 1;
    return 0;
}

int http_pkg_parse_pkg(const char *url, pkg_detail_t *out) {
    char err[160];
    return http_pkg_inspect(url, out, err, sizeof(err));
}

int http_pkg_inspect(const char *url, pkg_detail_t *out, char *err, size_t err_sz) {
    char canon[HTTP_PKG_URL_MAX];
    http_pkg_session_t *session = NULL;
    int rc;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (http_pkg_normalize(url, canon, sizeof(canon), err, err_sz) != 0) return -1;
    if (http_pkg_session_open(canon, &session, err, err_sz) != 0) {
        install_log("[HTTP] remote package %.160s: %s", canon, err && err[0] ? err : "open failed");
        return -1;
    }
    /* session->url is the link that returned the package bytes. Redirects
     * are already followed. Passthrough must queue this link: the PS5
     * installer does not follow redirects itself. */
    rc = parse_session(session, session->url, out);
    http_pkg_session_close(session);
    if (rc != 0) {
        set_err(err, err_sz, "Could not read package metadata from that link");
        install_log("[HTTP] remote package %.160s: metadata unreadable", canon);
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return 0;
}

int http_pkg_get_icon(const char *url, uint8_t **out_data, size_t *out_size) {
    pkg_detail_t detail;
    uint8_t *buf;
    ssize_t n;
    if (!url || !out_data || !out_size) return -1;
    *out_data = NULL;
    *out_size = 0;
    if (http_pkg_parse_pkg(url, &detail) != 0 || !detail.has_icon || detail.icon_size == 0 ||
        detail.icon_size >= 10 * 1024 * 1024) return -1;
    buf = malloc(detail.icon_size);
    if (!buf) return -1;
    n = http_pkg_pread(url, buf, detail.icon_size, detail.icon_offset);
    if (n == (ssize_t)detail.icon_size) {
        *out_data = buf;
        *out_size = detail.icon_size;
        return 0;
    }
    free(buf);
    return -1;
}
