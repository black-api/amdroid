/* Small HTTP/1.1 server: an accept thread feeds a bounded queue served by a
 * pool of worker threads. Keep-alive is supported (and the default for
 * HTTP/1.1); streaming (SSE) responses close the connection when done. */
#ifndef THIMBLE_HTTP_H
#define THIMBLE_HTTP_H

#include <stddef.h>

typedef struct {
    int    fd;
    int    worker;              /* index of the worker thread serving this request */
    char   method[16];
    char   path[512];           /* without the query string */
    char   query[512];          /* after '?', may be empty */
    char   authorization[512];  /* Authorization header value, may be empty */
    char   remote[64];          /* client address */
    int    keep_alive;          /* set by the parser; a handler may clear it */
    char  *body;                /* NUL-terminated */
    size_t body_len;
    const char *cors;           /* Access-Control-Allow-Origin value (NULL = none) */
} http_request;

typedef void (*http_handler)(http_request *req, void *user);

typedef struct {
    const char *host;
    int         port;
    int         n_workers;      /* concurrent requests */
    int         queue_size;     /* pending connections before answering 503 */
    int         keepalive_s;    /* idle keep-alive timeout; 0 disables keep-alive */
    int         timeout_s;      /* read timeout for a request */
    size_t      max_body;       /* bytes */
    const char *cors;           /* NULL disables CORS headers */
} http_options;

/* Serve forever; returns -1 on setup failure. */
int http_serve(const http_options *opt, http_handler handler, void *user);

/* Responses (exactly one per request). */
int http_send(http_request *req, int status, const char *content_type, const char *body, size_t len);
int http_send_json(http_request *req, int status, const char *json);
int http_send_error(http_request *req, int status, const char *message);

/* Server-sent events: headers once, then one event per call. */
int http_sse_begin(http_request *req);
int http_sse_event(http_request *req, const char *data, size_t len);   /* "data: ...\n\n" */

#endif
