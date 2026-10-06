/**
 * @file http_server.c
 * @brief Single-client HTTP server for the live MJPEG stream, driven by the main poll() loop.
 *
 * Socket setup follows the aesdsocket assignment. The client moves through:
 *   READING  - collecting the request headers
 *   SENDING  - writing a one-off response (page, snapshot, error), then closing
 *   STREAMING - writing one multipart JPEG part per new frame until the client disconnects
 */

/* For accept4() */
#define _GNU_SOURCE

#include "http_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define LISTEN_BACKLOG              8
#define REQUEST_MAX                 4096
/* A client must send its request within this time */
#define REQUEST_TIMEOUT_SECONDS     5
/* A client that accepts no data for this long is dropped */
#define SEND_TIMEOUT_SECONDS        10

#define STREAM_BOUNDARY             "frame"

static const char index_page[] =
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    /* An empty icon stops the browser using a second connection to fetch favicon.ico */
    "<link rel=\"icon\" href=\"data:,\">\n"
    "<title>camera-server</title>\n"
    "</head>\n"
    "<body style=\"margin:0;background:#000\">\n"
    "<img src=\"/stream\" alt=\"Live camera stream\" style=\"display:block;width:100%;height:auto\">\n"
    "</body>\n"
    "</html>\n";

enum client_state {
    CLIENT_NONE,
    CLIENT_READING,
    CLIENT_SENDING,
    CLIENT_STREAMING,
};

struct http_client {
    int fd;
    enum client_state state;
    char addr[INET_ADDRSTRLEN];
    char request[REQUEST_MAX];
    size_t request_len;
    unsigned char *out;     /* pending output */
    size_t out_len;
    size_t out_sent;
    size_t out_capacity;
    double deadline;        /* request or send timeout, monotonic seconds */
    unsigned long frames_sent;
    unsigned long frames_skipped;
};

struct http_server {
    int listen_fd;
    struct http_client client;
};

static double monotonic_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void client_reset(struct http_client *c)
{
    free(c->out);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->state = CLIENT_NONE;
}

static void client_disconnect(struct http_client *c, const char *reason)
{
    if (c->state == CLIENT_STREAMING) {
        syslog(LOG_INFO, "Client %s disconnected (%s): %lu frames sent, %lu skipped", c->addr,
               reason, c->frames_sent, c->frames_skipped);
    } else {
        syslog(LOG_INFO, "Client %s disconnected (%s)", c->addr, reason);
    }
    close(c->fd);
    client_reset(c);
}

/** Append to the pending output buffer. */
static int out_append(struct http_client *c, const void *data, size_t len)
{
    if (c->out_len + len > c->out_capacity) {
        size_t capacity = (c->out_len + len) * 3 / 2;
        unsigned char *buf = realloc(c->out, capacity);

        if (buf == NULL) {
            syslog(LOG_ERR, "Out of memory for client output");
            return -1;
        }
        c->out = buf;
        c->out_capacity = capacity;
    }
    memcpy(c->out + c->out_len, data, len);
    c->out_len += len;
    return 0;
}

/**
 * Write as much pending output as the socket accepts.
 * @return 0 if the client is still connected, -1 if it was disconnected
 */
static int client_flush(struct http_client *c)
{
    while (c->out_sent < c->out_len) {
        ssize_t n = send(c->fd, c->out + c->out_sent, c->out_len - c->out_sent, MSG_NOSIGNAL);

        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            client_disconnect(c, strerror(errno));
            return -1;
        }
        c->out_sent += (size_t)n;
        c->deadline = monotonic_seconds() + SEND_TIMEOUT_SECONDS;
    }

    /* Everything written */
    c->out_len = 0;
    c->out_sent = 0;
    if (c->state == CLIENT_SENDING) {
        client_disconnect(c, "response sent");
        return -1;
    }
    return 0;
}

static int out_pending(const struct http_client *c)
{
    return c->out_sent < c->out_len;
}

/** Queue a complete response and switch to SENDING; the connection closes once it is sent. */
static void queue_response(struct http_client *c, const char *status, const char *content_type,
                           const void *body, size_t body_len)
{
    char header[256];
    int n;

    n = snprintf(header, sizeof(header),
                 "HTTP/1.1 %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Cache-Control: no-cache, no-store\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 status, content_type, body_len);
    c->state = CLIENT_SENDING;
    c->out_len = 0;
    c->out_sent = 0;
    c->deadline = monotonic_seconds() + SEND_TIMEOUT_SECONDS;
    if (out_append(c, header, (size_t)n) == -1 || out_append(c, body, body_len) == -1) {
        client_disconnect(c, "out of memory");
        return;
    }
    client_flush(c);
}

static void queue_error(struct http_client *c, const char *status)
{
    char body[128];
    int n = snprintf(body, sizeof(body), "%s\n", status);

    queue_response(c, status, "text/plain", body, (size_t)n);
}

static void start_stream(struct http_client *c)
{
    static const char header[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=" STREAM_BOUNDARY "\r\n"
        "Cache-Control: no-cache, no-store\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";

    c->state = CLIENT_STREAMING;
    c->out_len = 0;
    c->out_sent = 0;
    c->deadline = monotonic_seconds() + SEND_TIMEOUT_SECONDS;
    if (out_append(c, header, sizeof(header) - 1) == -1) {
        client_disconnect(c, "out of memory");
        return;
    }
    syslog(LOG_INFO, "Client %s streaming", c->addr);
    client_flush(c);
}

/** Parse the request line and queue the matching response. */
static void handle_request(struct http_client *c, const struct frame_store *store)
{
    char method[8];
    char path[256];
    char version[16];
    char *query;

    if (sscanf(c->request, "%7s %255s %15s", method, path, version) != 3 ||
        strncmp(version, "HTTP/1.", 7) != 0 || path[0] != '/') {
        syslog(LOG_INFO, "Client %s sent a malformed request", c->addr);
        queue_error(c, "400 Bad Request");
        return;
    }

    query = strchr(path, '?');
    if (query != NULL) {
        *query = '\0';
    }
    syslog(LOG_INFO, "Client %s: %s %s", c->addr, method, path);

    if (strcmp(method, "GET") != 0) {
        queue_error(c, "405 Method Not Allowed");
    } else if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        queue_response(c, "200 OK", "text/html; charset=utf-8", index_page,
                       sizeof(index_page) - 1);
    } else if (strcmp(path, "/stream") == 0) {
        start_stream(c);
    } else if (strcmp(path, "/snapshot.jpg") == 0) {
        if (store->seq == 0) {
            queue_error(c, "503 Service Unavailable");
        } else {
            queue_response(c, "200 OK", "image/jpeg", store->data, store->len);
        }
    } else {
        queue_error(c, "404 Not Found");
    }
}

/** Read request bytes; once the headers are complete, handle the request. */
static void client_read(struct http_client *c, const struct frame_store *store)
{
    for (;;) {
        size_t space = sizeof(c->request) - 1 - c->request_len;
        ssize_t n;

        if (space == 0) {
            syslog(LOG_INFO, "Client %s request too large", c->addr);
            queue_error(c, "400 Bad Request");
            return;
        }

        n = recv(c->fd, c->request + c->request_len, space, 0);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                client_disconnect(c, strerror(errno));
            }
            return;
        }
        if (n == 0) {
            client_disconnect(c, "closed by client");
            return;
        }

        c->request_len += (size_t)n;
        c->request[c->request_len] = '\0';
        if (strstr(c->request, "\r\n\r\n") != NULL) {
            handle_request(c, store);
            return;
        }
    }
}

/** While sending or streaming, only watch for the client closing the connection. */
static void client_discard_input(struct http_client *c)
{
    char buf[512];

    for (;;) {
        ssize_t n = recv(c->fd, buf, sizeof(buf), 0);

        if (n > 0) {
            continue;
        }
        if (n == -1 && errno == EINTR) {
            continue;
        }
        if (n == 0) {
            client_disconnect(c, "closed by client");
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            client_disconnect(c, strerror(errno));
        }
        return;
    }
}

/** Accept every waiting connection: the first becomes the client, the rest get 503. */
static void accept_connections(struct http_server *srv)
{
    static const char busy[] =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 31\r\n"
        "Retry-After: 5\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Another client is being served\n";

    for (;;) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        char addr_str[INET_ADDRSTRLEN];
        int fd;

        fd = accept4(srv->listen_fd, (struct sockaddr *)&addr, &addr_len, SOCK_NONBLOCK);
        if (fd == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            }
            return;
        }
        inet_ntop(AF_INET, &addr.sin_addr, addr_str, sizeof(addr_str));

        if (srv->client.state != CLIENT_NONE) {
            /* Best effort: the response is small enough for an empty socket buffer */
            if (send(fd, busy, sizeof(busy) - 1, MSG_NOSIGNAL) == -1) {
                syslog(LOG_INFO, "Sending 503 to %s failed: %s", addr_str, strerror(errno));
            }
            syslog(LOG_INFO, "Rejected %s with 503: client %s is being served", addr_str,
                   srv->client.addr);
            close(fd);
            continue;
        }

        srv->client.fd = fd;
        srv->client.state = CLIENT_READING;
        srv->client.deadline = monotonic_seconds() + REQUEST_TIMEOUT_SECONDS;
        memcpy(srv->client.addr, addr_str, sizeof(addr_str));
        syslog(LOG_INFO, "Accepted connection from %s", addr_str);
    }
}

struct http_server *http_server_open(unsigned short port)
{
    struct http_server *srv;
    struct sockaddr_in addr;
    int opt = 1;

    srv = calloc(1, sizeof(*srv));
    if (srv == NULL) {
        syslog(LOG_ERR, "Out of memory");
        return NULL;
    }
    client_reset(&srv->client);

    srv->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (srv->listen_fd == -1) {
        syslog(LOG_ERR, "socket failed: %s", strerror(errno));
        free(srv);
        return NULL;
    }
    if (setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        syslog(LOG_ERR, "setsockopt SO_REUSEADDR failed: %s", strerror(errno));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        syslog(LOG_ERR, "bind to port %u failed: %s", port, strerror(errno));
        close(srv->listen_fd);
        free(srv);
        return NULL;
    }
    if (listen(srv->listen_fd, LISTEN_BACKLOG) == -1) {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        close(srv->listen_fd);
        free(srv);
        return NULL;
    }

    syslog(LOG_INFO, "HTTP server listening on port %u", port);
    return srv;
}

int http_server_pollfds(struct http_server *srv, struct pollfd *fds)
{
    struct http_client *c = &srv->client;
    int count = 0;

    fds[count].fd = srv->listen_fd;
    fds[count].events = POLLIN;
    fds[count].revents = 0;
    count++;

    if (c->state != CLIENT_NONE) {
        fds[count].fd = c->fd;
        fds[count].events = POLLIN;
        if (out_pending(c)) {
            fds[count].events |= POLLOUT;
        }
        fds[count].revents = 0;
        count++;
    }
    return count;
}

void http_server_handle(struct http_server *srv, const struct pollfd *fds, int count,
                        const struct frame_store *store)
{
    struct http_client *c = &srv->client;
    int i;

    for (i = 0; i < count; i++) {
        if (fds[i].revents == 0) {
            continue;
        }

        if (fds[i].fd == srv->listen_fd) {
            accept_connections(srv);
            continue;
        }

        /* Skip results for a client that has already been replaced or dropped */
        if (c->state == CLIENT_NONE || fds[i].fd != c->fd) {
            continue;
        }

        if (fds[i].revents & POLLERR) {
            client_disconnect(c, "socket error");
            continue;
        }
        if ((fds[i].revents & POLLOUT) && client_flush(c) == -1) {
            continue;
        }
        if (fds[i].revents & (POLLIN | POLLHUP)) {
            if (c->state == CLIENT_READING) {
                client_read(c, store);
            } else {
                client_discard_input(c);
            }
        }
    }
}

void http_server_new_frame(struct http_server *srv, const struct frame_store *store)
{
    struct http_client *c = &srv->client;
    char header[128];
    int n;

    if (c->state != CLIENT_STREAMING) {
        return;
    }

    /* Still sending an earlier frame: skip this one so a slow client never falls behind */
    if (out_pending(c)) {
        c->frames_skipped++;
        return;
    }

    n = snprintf(header, sizeof(header),
                 "--" STREAM_BOUNDARY "\r\n"
                 "Content-Type: image/jpeg\r\n"
                 "Content-Length: %zu\r\n"
                 "\r\n",
                 store->len);
    c->out_len = 0;
    c->out_sent = 0;
    if (out_append(c, header, (size_t)n) == -1 || out_append(c, store->data, store->len) == -1 ||
        out_append(c, "\r\n", 2) == -1) {
        client_disconnect(c, "out of memory");
        return;
    }
    c->frames_sent++;
    client_flush(c);
}

void http_server_check_timeouts(struct http_server *srv)
{
    struct http_client *c = &srv->client;
    double now = monotonic_seconds();

    if (c->state == CLIENT_NONE) {
        return;
    }
    if (c->state == CLIENT_READING && now > c->deadline) {
        client_disconnect(c, "request timeout");
    } else if (out_pending(c) && now > c->deadline) {
        client_disconnect(c, "send timeout");
    }
}

void http_server_close(struct http_server *srv)
{
    if (srv == NULL) {
        return;
    }
    if (srv->client.state != CLIENT_NONE) {
        client_disconnect(&srv->client, "server shutting down");
    }
    close(srv->listen_fd);
    free(srv);
}
