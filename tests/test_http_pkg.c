/*
 * Host test for remote http:// package links: URL checks, ranged reads,
 * redirects, and metadata that matches the file parser.
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "http_pkg.h"
#include "multipart.h"
#include "pkg_parser.h"
#include "test_fixture.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

typedef struct {
    int fd;
    int port;
    uint8_t *data;
    size_t len;
    int mode; /* 0 = ranges, 1 = ignore ranges, 2 = redirect other paths to /pkg */
    volatile int stop;
    pthread_t thread;
} http_srv_t;

static int send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static void *serve(void *arg) {
    http_srv_t *srv = arg;
    while (!srv->stop) {
        int conn = accept(srv->fd, NULL, NULL);
        char req[4096];
        size_t n = 0;
        if (conn < 0) break;
        req[0] = '\0';
        struct timeval tv = {2, 0};
        setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        for (;;) {
            char *end;
            char method[16], path[512];
            unsigned long long start = 0, stop_at = 0;
            int have_range = 0;
            while (n + 1 < sizeof(req) && !strstr(req, "\r\n\r\n")) {
                ssize_t r = recv(conn, req + n, sizeof(req) - 1 - n, 0);
                if (r < 0 && errno == EINTR) continue;
                if (r <= 0) {
                    n = 0;
                    break;
                }
                n += (size_t)r;
                req[n] = '\0';
            }
            if (n == 0 || !strstr(req, "\r\n\r\n")) break;
            end = strstr(req, "\r\n\r\n");
            if (sscanf(req, "%15s %511s", method, path) != 2) break;
            {
                char *range = strstr(req, "\r\nRange:");
                if (range && sscanf(range, "\r\nRange: bytes=%llu-%llu", &start, &stop_at) == 2) have_range = 1;
            }
            if (srv->mode == 2 && strncmp(path, "/pkg", 4) != 0) {
                const char *redir = "HTTP/1.1 302 Found\r\nLocation: /pkg\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                send_all(conn, redir, strlen(redir));
                break;
            }
            if (srv->mode == 3 && strncmp(path, "/dir/HttpGame.pkg", 17) != 0) {
                const char *missing = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                send_all(conn, missing, strlen(missing));
                break;
            }
            if (srv->mode == 3 && !strstr(path, "ok=1")) {
                const char *redir = "HTTP/1.1 302 Found\r\nLocation: ?ok=1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                send_all(conn, redir, strlen(redir));
                break;
            }
            if (srv->mode == 1 || !have_range) {
                char hdr[128];
                int hn = snprintf(hdr, sizeof(hdr),
                                  "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                                  srv->len);
                send_all(conn, hdr, (size_t)hn);
                send_all(conn, srv->data, srv->len);
                break;
            }
            if (start >= srv->len || stop_at >= srv->len || stop_at < start) {
                const char *bad = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                send_all(conn, bad, strlen(bad));
                break;
            }
            {
                size_t body = (size_t)(stop_at - start + 1);
                char hdr[192];
                int hn = snprintf(hdr, sizeof(hdr),
                                  "HTTP/1.1 206 Partial Content\r\n"
                                  "Content-Range: bytes %llu-%llu/%zu\r\n"
                                  "Content-Length: %zu\r\n"
                                  "Connection: keep-alive\r\n"
                                  "\r\n",
                                  start, stop_at, srv->len, body);
                if (send_all(conn, hdr, (size_t)hn) != 0 ||
                    send_all(conn, srv->data + start, body) != 0) break;
            }
            {
                size_t used = (size_t)(end + 4 - req);
                memmove(req, end + 4, n - used);
                n -= used;
                req[n] = '\0';
            }
        }
        close(conn);
    }
    return NULL;
}

static int serve_start(http_srv_t *srv, uint8_t *data, size_t len, int mode) {
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int one = 1;
    memset(srv, 0, sizeof(*srv));
    srv->data = data;
    srv->len = len;
    srv->mode = mode;
    srv->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->fd < 0) return -1;
    setsockopt(srv->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(srv->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(srv->fd, 8) != 0 ||
        getsockname(srv->fd, (struct sockaddr *)&addr, &alen) != 0) {
        close(srv->fd);
        return -1;
    }
    srv->port = ntohs(addr.sin_port);
    if (pthread_create(&srv->thread, NULL, serve, srv) != 0) {
        close(srv->fd);
        return -1;
    }
    return 0;
}

static void serve_stop(http_srv_t *srv) {
    srv->stop = 1;
    shutdown(srv->fd, SHUT_RDWR);
    close(srv->fd);
    pthread_join(srv->thread, NULL);
}

static uint8_t *load_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *buf;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

int main(void) {
    char canon[HTTP_PKG_URL_MAX], err[160], name[64], url[HTTP_PKG_URL_MAX];
    char too_long[700];
    const char *fixture = "/tmp/http_pkg_fixture.pkg";
    uint8_t *data = NULL;
    size_t len = 0;
    http_srv_t srv;
    pkg_detail_t file, remote;
    uint8_t slice[32];
    virtual_stream_t stream;

    printf("Testing remote http link validation...\n");
    assert(http_pkg_normalize(" HTTPS://example.com/a.pkg#part ", canon, sizeof(canon), err, sizeof(err)) == 0);
    assert(strcmp(canon, "https://example.com/a.pkg") == 0);
    assert(http_pkg_normalize("https://example.com:443/a.pkg", canon, sizeof(canon), err, sizeof(err)) == 0);
    assert(strcmp(canon, "https://example.com/a.pkg") == 0);
    assert(http_pkg_is_url("https://example.com/a.pkg") == 1);
    assert(http_pkg_is_url("http://user:secret@example.com/a.pkg") == 0);
    assert(http_pkg_is_url("ftp://example.com/a.pkg") == 0);
    assert(http_pkg_normalize("  HTTP://example.com/Game.pkg#part  ", canon, sizeof(canon), err, sizeof(err)) == 0);
    assert(strcmp(canon, "http://example.com/Game.pkg") == 0);
    assert(http_pkg_normalize("http://example.com:80/a.pkg", canon, sizeof(canon), err, sizeof(err)) == 0);
    assert(strcmp(canon, "http://example.com/a.pkg") == 0);
    http_pkg_filename("http://nas.local/dir/My%20Game.pkg?token=1", name, sizeof(name));
    assert(strcmp(name, "My Game.pkg") == 0);
    memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    memcpy(too_long, "http://nas/", 11);
    assert(http_pkg_normalize(too_long, canon, sizeof(canon), err, sizeof(err)) != 0);

    printf("Testing ranged download and package metadata...\n");
    assert(fixture_write_ps5_pkg(fixture, "PPSA90012", "HttpGame", "gd", "01.000.000", 1) == 0);
    data = load_file(fixture, &len);
    assert(data && len > 64);
    assert(serve_start(&srv, data, len, 0) == 0);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/dir/HttpGame.pkg?token=1", srv.port);
    assert(pkg_parser_parse(fixture, &file) == 0);
    assert(http_pkg_parse_pkg(url, &remote) == 0);
    assert(strcmp(remote.title_id, file.title_id) == 0);
    assert(strcmp(remote.title_name, file.title_name) == 0);
    assert(strcmp(remote.pkg_type_str, file.pkg_type_str) == 0);
    assert(strcmp(remote.content_id, file.content_id) == 0);
    assert(strcmp(remote.app_version, file.app_version) == 0);
    assert(remote.file_size == file.file_size);
    assert(remote.has_icon == file.has_icon);
    assert(strcmp(remote.filename, "HttpGame.pkg") == 0);
    assert(http_pkg_pread(url, slice, sizeof(slice), 16) == (ssize_t)sizeof(slice));
    assert(memcmp(slice, data + 16, sizeof(slice)) == 0);
    memset(&stream, 0, sizeof(stream));
    assert(virtual_stream_check_path(url) == 0);
    assert(virtual_stream_open(url, &stream) == 0);
    assert(stream.is_http == 1);
    assert(stream.total_pkg_size == len);
    assert(virtual_stream_read(&stream, 0, slice, sizeof(slice)) == (ssize_t)sizeof(slice));
    assert(memcmp(slice, data, sizeof(slice)) == 0);
    virtual_stream_close(&stream);
    serve_stop(&srv);

    printf("Testing redirect and servers without ranges...\n");
    assert(serve_start(&srv, data, len, 2) == 0);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/go", srv.port);
    assert(http_pkg_parse_pkg(url, &remote) == 0);
    assert(strcmp(remote.title_id, "PPSA90012") == 0);
    serve_stop(&srv);

    assert(serve_start(&srv, data, len, 3) == 0);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/dir/HttpGame.pkg", srv.port);
    assert(http_pkg_parse_pkg(url, &remote) == 0);
    assert(strcmp(remote.title_id, "PPSA90012") == 0);
    serve_stop(&srv);

    assert(serve_start(&srv, data, len, 1) == 0);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/HttpGame.pkg", srv.port);
    err[0] = '\0';
    assert(http_pkg_inspect(url, &remote, err, sizeof(err)) != 0);
    assert(strstr(err, "range"));
    serve_stop(&srv);

    free(data);
    unlink(fixture);
    printf("Remote http link tests passed.\n");
    return 0;
}
