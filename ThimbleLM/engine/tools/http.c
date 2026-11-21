#include "http.h"
#include "json.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_HEADER (64 * 1024)

/* ---- responses ---------------------------------------------------------- */

static int send_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w, n -= (size_t) w;
    }
    return 0;
}

static const char *status_text(int s) {
    switch (s) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 503: return "Service Unavailable";
        default:  return "Internal Server Error";
    }
}

static void cors_headers(const http_request *req, sbuf *b) {
    if (!req->cors) return;
    sb_puts(b, "Access-Control-Allow-Origin: ");
    sb_puts(b, req->cors);
    sb_puts(b, "\r\nAccess-Control-Allow-Headers: Authorization, Content-Type\r\n"
               "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n");
}

int http_send(http_request *req, int status, const char *ctype, const char *body, size_t len) {
    sbuf h;
    sb_init(&h);
    sb_printf(&h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: %s\r\n", status,
              status_text(status), ctype, len, req->keep_alive ? "keep-alive" : "close");
    if (status == 401) sb_puts(&h, "WWW-Authenticate: Bearer\r\n");
    cors_headers(req, &h);
    sb_puts(&h, "\r\n");
    int rc = send_all(req->fd, h.s, h.n);
    sb_free(&h);
    if (rc == 0 && len) rc = send_all(req->fd, body, len);
    if (rc) req->keep_alive = 0;
    return rc;
}

int http_send_json(http_request *req, int status, const char *js) {
    return http_send(req, status, "application/json", js, strlen(js));
}

int http_send_error(http_request *req, int status, const char *message) {
    sbuf b;
    sb_init(&b);
    sb_puts(&b, "{\"error\":{\"message\":");
    sb_json_str(&b, message, strlen(message));
    const char *type = status == 400 ? "invalid_request_error" : status == 401 ? "authentication_error"
                     : status == 404 ? "not_found_error" : status == 503 ? "overloaded_error" : "server_error";
    sb_printf(&b, ",\"type\":\"%s\",\"code\":%d}}", type, status);
    int rc = http_send_json(req, status, b.s);
    sb_free(&b);
    return rc;
}

int http_sse_begin(http_request *req) {
    sbuf h;
    sb_init(&h);
    sb_puts(&h, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
                "Connection: close\r\n");
    cors_headers(req, &h);
    sb_puts(&h, "\r\n");
    req->keep_alive = 0;   /* the stream ends by closing the connection */
    int rc = send_all(req->fd, h.s, h.n);
    sb_free(&h);
    return rc;
}

int http_sse_event(http_request *req, const char *data, size_t len) {
    if (send_all(req->fd, "data: ", 6) || send_all(req->fd, data, len)) return -1;
    return send_all(req->fd, "\n\n", 2);
}

/* ---- request parsing (per connection, keeps pipelined leftovers) ---------- */

typedef struct {
    int    fd;
    char  *buf;
    size_t len, cap;
} conn;

static int conn_fill(conn *c) {
    if (c->len + 4096 > c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 16384;
        char  *nb  = realloc(c->buf, cap + 1);
        if (!nb) return -1;
        c->buf = nb, c->cap = cap;
    }
    ssize_t r;
    do r = recv(c->fd, c->buf + c->len, c->cap - c->len, 0); while (r < 0 && errno == EINTR);
    if (r <= 0) return -1;
    c->len += (size_t) r;
    c->buf[c->len] = '\0';
    return 0;
}

static void header_value(const char *line, size_t name_len, char *out, size_t out_len) {
    const char *v = line + name_len;
    while (*v == ' ' || *v == '\t') v++;
    size_t n = 0;
    while (v[n] && v[n] != '\r' && n + 1 < out_len) n++;
    memcpy(out, v, n);
    out[n] = '\0';
}

/* 0: request parsed; >0: HTTP error status to send; -1: connection closed / I/O error */
static int read_request(conn *c, http_request *req, size_t max_body) {
    char *end;
    while (!(c->buf && (end = strstr(c->buf, "\r\n\r\n")))) {
        if (c->len >= MAX_HEADER) return 413;
        if (conn_fill(c)) return -1;
    }
    size_t head_len = (size_t) (end + 4 - c->buf);
    char   version[16] = "", target[1024] = "";
    if (sscanf(c->buf, "%15s %1023s %15s", req->method, target, version) != 3) return 400;
    char *q = strchr(target, '?');
    if (q) { *q = '\0'; snprintf(req->query, sizeof(req->query), "%s", q + 1); }
    snprintf(req->path, sizeof(req->path), "%s", target);
    req->keep_alive = !strcmp(version, "HTTP/1.1");

    size_t content_length = 0;
    for (char *line = strstr(c->buf, "\r\n"); line && line < end; line = strstr(line + 2, "\r\n")) {
        char *h = line + 2;
        char  v[512];
        if (!strncasecmp(h, "Content-Length:", 15)) content_length = (size_t) strtoull(h + 15, NULL, 10);
        else if (!strncasecmp(h, "Authorization:", 14)) header_value(h, 14, req->authorization, sizeof(req->authorization));
        else if (!strncasecmp(h, "Connection:", 11)) {
            header_value(h, 11, v, sizeof(v));
            if (!strcasecmp(v, "close")) req->keep_alive = 0;
            if (!strcasecmp(v, "keep-alive")) req->keep_alive = 1;
        }
    }
    if (content_length > max_body) { req->keep_alive = 0; return 413; }
    while (c->len < head_len + content_length)
        if (conn_fill(c)) return -1;

    req->body = malloc(content_length + 1);
    if (!req->body) return -1;
    memcpy(req->body, c->buf + head_len, content_length);
    req->body[content_length] = '\0';
    req->body_len = content_length;
    size_t used = head_len + content_length;   /* keep any pipelined bytes */
    memmove(c->buf, c->buf + used, c->len - used);
    c->len -= used;
    c->buf[c->len] = '\0';
    return 0;
}

/* ---- connection queue + workers --------------------------------------------- */

typedef struct {
    int  fd;
    char remote[64];
} pending;

typedef struct {
    const http_options *opt;
    http_handler        handler;
    void               *user;
    pending            *q;
    int                 head, count;
    pthread_mutex_t     mu;
    pthread_cond_t      cv;
} server_state;

typedef struct {
    server_state *st;
    int           index;
} worker_arg;

static void set_timeout(int fd, int seconds) {
    struct timeval tv = { seconds, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static void serve_connection(server_state *st, int worker, pending p) {
    const http_options *opt = st->opt;
    conn c = { p.fd, NULL, 0, 0 };
    set_timeout(p.fd, opt->timeout_s);
    for (int first = 1;; first = 0) {
        if (!first) {
            if (opt->keepalive_s <= 0) break;
            set_timeout(p.fd, opt->keepalive_s);   /* idle wait for the next request */
            if (!c.len && conn_fill(&c)) break;
            set_timeout(p.fd, opt->timeout_s);
        }
        http_request req;
        memset(&req, 0, sizeof(req));
        req.fd = p.fd, req.worker = worker, req.cors = opt->cors;
        snprintf(req.remote, sizeof(req.remote), "%s", p.remote);
        int rc = read_request(&c, &req, opt->max_body);
        if (rc == 0) {
            if (opt->keepalive_s <= 0) req.keep_alive = 0;
            st->handler(&req, st->user);
        } else if (rc > 0) {
            req.keep_alive = 0;
            http_send_error(&req, rc, rc == 413 ? "request too large" : "malformed request");
        }
        free(req.body);
        if (rc != 0 || !req.keep_alive) break;
    }
    free(c.buf);
    close(p.fd);
}

static void *worker_main(void *arg) {
    worker_arg   *wa = arg;
    server_state *st = wa->st;
    for (;;) {
        pthread_mutex_lock(&st->mu);
        while (st->count == 0) pthread_cond_wait(&st->cv, &st->mu);
        pending p = st->q[st->head];
        st->head = (st->head + 1) % st->opt->queue_size;
        st->count--;
        pthread_mutex_unlock(&st->mu);
        serve_connection(st, wa->index, p);
    }
    return NULL;
}

int http_serve(const http_options *opt, http_handler handler, void *user) {
    signal(SIGPIPE, SIG_IGN);
    struct addrinfo hints = { 0 }, *ai = NULL;
    char port_s[16];
    snprintf(port_s, sizeof(port_s), "%d", opt->port);
    hints.ai_family = AF_UNSPEC, hints.ai_socktype = SOCK_STREAM, hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(opt->host, port_s, &hints, &ai) != 0 || !ai) {
        fprintf(stderr, "cannot resolve %s\n", opt->host);
        return -1;
    }
    int srv = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    int one = 1;
    if (srv >= 0) setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (srv < 0 || bind(srv, ai->ai_addr, ai->ai_addrlen) != 0 || listen(srv, 64) != 0) {
        fprintf(stderr, "cannot listen on %s:%d: %s\n", opt->host, opt->port, strerror(errno));
        freeaddrinfo(ai);
        if (srv >= 0) close(srv);
        return -1;
    }
    freeaddrinfo(ai);

    server_state st = { opt, handler, user, calloc((size_t) opt->queue_size, sizeof(pending)), 0, 0,
                        PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER };
    worker_arg *args = calloc((size_t) opt->n_workers, sizeof(worker_arg));
    for (int i = 0; i < opt->n_workers; i++) {
        pthread_t t;
        args[i] = (worker_arg) { &st, i };
        pthread_create(&t, NULL, worker_main, &args[i]);
        pthread_detach(t);
    }
    fprintf(stderr, "thimble server listening on http://%s:%d (%d workers)\n", opt->host, opt->port,
            opt->n_workers);

    for (;;) {
        struct sockaddr_storage addr;
        socklen_t               alen = sizeof(addr);
        int fd = accept(srv, (struct sockaddr *) &addr, &alen);
        if (fd < 0) {
            if (errno != EINTR) perror("accept");
            continue;
        }
        pending p = { fd, "" };
        if (addr.ss_family == AF_INET)
            inet_ntop(AF_INET, &((struct sockaddr_in *) &addr)->sin_addr, p.remote, sizeof(p.remote));
        else if (addr.ss_family == AF_INET6)
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *) &addr)->sin6_addr, p.remote, sizeof(p.remote));

        pthread_mutex_lock(&st.mu);
        int full = st.count == opt->queue_size;
        if (!full) {
            st.q[(st.head + st.count) % opt->queue_size] = p;
            st.count++;
            pthread_cond_signal(&st.cv);
        }
        pthread_mutex_unlock(&st.mu);
        if (full) {   /* overloaded: answer immediately instead of queueing forever */
            http_request req;
            memset(&req, 0, sizeof(req));
            req.fd = fd, req.cors = opt->cors;
            http_send_error(&req, 503, "server busy, try again");
            close(fd);
        }
    }
}
