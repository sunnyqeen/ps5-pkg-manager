#ifndef HTTP_PKG_H
#define HTTP_PKG_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "pkg_parser.h"

/* Queued install paths are 511 characters plus a NUL. */
#define HTTP_PKG_URL_MAX 512

/* Opaque keep-alive client for one remote package. */
typedef struct http_pkg_session http_pkg_session_t;

/* 1 when url is an http:// or https:// package link that fits in an install path. */
int http_pkg_is_url(const char *url);

/* Trim, strip a fragment, and rewrite the scheme in lowercase.
 * out receives a canonical link of at most HTTP_PKG_URL_MAX-1 bytes. */
int http_pkg_normalize(const char *url, char *out, size_t out_sz, char *err, size_t err_sz);

/* Last path segment, percent-decoded, or "package.pkg". */
void http_pkg_filename(const char *url, char *out, size_t out_sz);

/* Connect, require byte ranges, and remember the file size.
 * On the PS5, libSceHttp fetches http:// and https://. err is optional. */
int http_pkg_session_open(const char *url, http_pkg_session_t **out, char *err, size_t err_sz);
ssize_t http_pkg_session_read(http_pkg_session_t *session, void *buf, size_t count, uint64_t offset);
/* Receives each chunk of a streamed range. Return 0 to continue, nonzero to abort. */
typedef int (*http_pkg_sink_fn)(void *ctx, const void *data, size_t len);

/* Forward the raw range [start,end] as ONE upstream request. The body is
 * delivered to sink in pieces of at most buf_sz bytes through buf. Small
 * reads via http_pkg_session_read share one cached metadata block. */
int http_pkg_session_stream(http_pkg_session_t *session, uint64_t start, uint64_t end,
                            void *buf, size_t buf_sz, http_pkg_sink_fn sink, void *ctx);
uint64_t http_pkg_session_size(const http_pkg_session_t *session);
void http_pkg_session_close(http_pkg_session_t *session);

/* One-shot range read. Returns the byte count, or -1. */
ssize_t http_pkg_pread(const char *url, void *buf, size_t count, uint64_t offset);

/* Random-access PKG parse. out->path is the link after redirects. */
int http_pkg_parse_pkg(const char *url, pkg_detail_t *out);

/* Parse plus a user-facing error. */
int http_pkg_inspect(const char *url, pkg_detail_t *out, char *err, size_t err_sz);

int http_pkg_get_icon(const char *url, uint8_t **out_data, size_t *out_size);

#endif /* HTTP_PKG_H */
