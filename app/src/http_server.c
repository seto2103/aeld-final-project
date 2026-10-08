/**
 * @file http_server.c
 * @brief HTTP server for the live MJPEG stream, with one thread per client.
 *
 * Socket setup and the client thread list follow the aesdsocket assignment. Only the main thread
 * touches the client list: it starts a thread for each accepted connection, and joins threads
 * once they mark themselves complete. Each client thread owns its blocking socket for reading
 * and writing, but the socket is closed by the main thread after the join, so shutdown() from
 * http_server_close() can never hit a reused descriptor.
 */

/* For accept4() */
#define _GNU_SOURCE

#include "http_server.h"
#include "thread_util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <syslog.h>
#include <unistd.h>

#define LISTEN_BACKLOG              8
#define REQUEST_MAX                 4096
/* A client must send its request within this time */
#define REQUEST_TIMEOUT_SECONDS     5
/* A client that accepts no data for this long is dropped */
#define SEND_TIMEOUT_SECONDS        10
/* How long a stream waits for a frame before checking again; capture exits on a real stall */
#define STREAM_WAIT_MS              1000
/* How long /snapshot.jpg waits for the first frame after startup */
#define SNAPSHOT_WAIT_MS            2000

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

struct http_client {
    pthread_t thread;
    int fd;
    atomic_int thread_complete;     /* set by the client thread as its last action */
    char addr[INET_ADDRSTRLEN];
    struct frame_store *store;
    SLIST_ENTRY(http_client) entries;
};

SLIST_HEAD(client_list, http_client);

struct http_server {
    int listen_fd;
    unsigned int max_clients;
    unsigned int num_clients;       /* entries in clients, including finished but not joined */
    struct frame_store *store;
    struct client_list clients;
};

/**
 * Send the whole buffer, retrying partial writes.
 * @param flags MSG_MORE when more data follows immediately
 * @return 0 on success, -1 if the client disconnected or stopped reading (logged)
 */
static int send_all(struct http_client *c, const void *data, size_t len, int flags)
{
    const unsigned char *p = data;

    while (len > 0) {
        ssize_t n = send(c->fd, p, len, MSG_NOSIGNAL | flags);

        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                syslog(LOG_INFO, "Client %s send timeout", c->addr);
            } else {
                syslog(LOG_INFO, "Client %s disconnected: %m", c->addr);
            }
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/** Send a complete response with a Content-Length; the connection is closed afterwards. */
static void send_response(struct http_client *c, const char *status, const char *content_type,
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
    if (send_all(c, header, (size_t)n, MSG_MORE) == 0) {
        send_all(c, body, body_len, 0);
    }
}

static void send_error(struct http_client *c, const char *status)
{
    char body[128];
    int n = snprintf(body, sizeof(body), "%s\n", status);

    send_response(c, status, "text/plain", body, (size_t)n);
}

static void send_snapshot(struct http_client *c)
{
    struct frame_copy frame = { 0 };

    /* after_seq 0: the newest frame right away, or wait briefly for the first one */
    if (frame_store_wait_newer(c->store, 0, &frame, SNAPSHOT_WAIT_MS) == 0) {
        send_response(c, "200 OK", "image/jpeg", frame.data, frame.len);
    } else {
        send_error(c, "503 Service Unavailable");
    }
    frame_copy_free(&frame);
}

/** Send one multipart JPEG part per new frame until the client leaves or the server stops. */
static void stream(struct http_client *c)
{
    static const char header[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=" STREAM_BOUNDARY "\r\n"
        "Cache-Control: no-cache, no-store\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";
    struct frame_copy frame = { 0 };
    unsigned long frames_sent = 0;
    unsigned long frames_skipped = 0;
    uint64_t last_seq = 0;

    if (send_all(c, header, sizeof(header) - 1, 0) == -1) {
        return;
    }
    syslog(LOG_INFO, "Client %s streaming", c->addr);

    for (;;) {
        char part_header[128];
        int ret = frame_store_wait_newer(c->store, last_seq, &frame, STREAM_WAIT_MS);
        int n;

        if (ret == 1) {
            continue;
        }
        if (ret == -1) {
            syslog(LOG_INFO, "Client %s stream ended: server stopping", c->addr);
            break;
        }

        /* Frames captured while this client was still sending the previous one */
        if (last_seq != 0) {
            frames_skipped += frame.seq - last_seq - 1;
        }
        last_seq = frame.seq;

        n = snprintf(part_header, sizeof(part_header),
                     "--" STREAM_BOUNDARY "\r\n"
                     "Content-Type: image/jpeg\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n",
                     frame.len);
        if (send_all(c, part_header, (size_t)n, MSG_MORE) == -1 ||
            send_all(c, frame.data, frame.len, MSG_MORE) == -1 ||
            send_all(c, "\r\n", 2, 0) == -1) {
            break;
        }
        frames_sent++;
    }

    syslog(LOG_INFO, "Client %s: %lu frames sent, %lu skipped", c->addr, frames_sent,
           frames_skipped);
    frame_copy_free(&frame);
}

/** Parse the request line and send the matching response. */
static void handle_request(struct http_client *c, char *request)
{
    char method[8];
    char path[256];
    char version[16];
    char *query;

    if (sscanf(request, "%7s %255s %15s", method, path, version) != 3 ||
        strncmp(version, "HTTP/1.", 7) != 0 || path[0] != '/') {
        syslog(LOG_INFO, "Client %s sent a malformed request", c->addr);
        send_error(c, "400 Bad Request");
        return;
    }

    query = strchr(path, '?');
    if (query != NULL) {
        *query = '\0';
    }
    syslog(LOG_INFO, "Client %s: %s %s", c->addr, method, path);

    if (strcmp(method, "GET") != 0) {
        send_error(c, "405 Method Not Allowed");
    } else if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        send_response(c, "200 OK", "text/html; charset=utf-8", index_page,
                      sizeof(index_page) - 1);
    } else if (strcmp(path, "/stream") == 0) {
        stream(c);
    } else if (strcmp(path, "/snapshot.jpg") == 0) {
        send_snapshot(c);
    } else {
        send_error(c, "404 Not Found");
    }
}

/**
 * Read until the end of the request headers.
 * @return 0 when a complete request is in buf, -1 if the client left or timed out (logged)
 */
static int read_request(struct http_client *c, char *buf, size_t size)
{
    size_t len = 0;

    for (;;) {
        ssize_t n;

        if (len == size - 1) {
            syslog(LOG_INFO, "Client %s request too large", c->addr);
            send_error(c, "400 Bad Request");
            return -1;
        }

        n = recv(c->fd, buf + len, size - 1 - len, 0);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                syslog(LOG_INFO, "Client %s request timeout", c->addr);
            } else {
                syslog(LOG_INFO, "Client %s disconnected: %m", c->addr);
            }
            return -1;
        }
        if (n == 0) {
            syslog(LOG_INFO, "Client %s closed the connection", c->addr);
            return -1;
        }

        len += (size_t)n;
        buf[len] = '\0';
        if (strstr(buf, "\r\n\r\n") != NULL) {
            return 0;
        }
    }
}

static void *client_thread(void *arg)
{
    struct http_client *c = arg;
    struct timeval rcv_timeout = { .tv_sec = REQUEST_TIMEOUT_SECONDS };
    struct timeval snd_timeout = { .tv_sec = SEND_TIMEOUT_SECONDS };
    char request[REQUEST_MAX];

    /* The socket is blocking; these timeouts make recv() and send() give up on a stalled client */
    if (setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout)) == -1 ||
        setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &snd_timeout, sizeof(snd_timeout)) == -1) {
        syslog(LOG_ERR, "Client %s setsockopt failed: %m", c->addr);
    } else if (read_request(c, request, sizeof(request)) == 0) {
        handle_request(c, request);
    }

    atomic_store(&c->thread_complete, 1);
    return NULL;
}

static void reject_busy(int fd, const char *addr)
{
    static const char busy[] =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 17\r\n"
        "Retry-After: 5\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Too many viewers\n";

    /* Best effort: the response is small enough for an empty socket buffer, so it won't block */
    if (send(fd, busy, sizeof(busy) - 1, MSG_NOSIGNAL | MSG_DONTWAIT) == -1) {
        syslog(LOG_INFO, "Sending 503 to %s failed: %m", addr);
    }
    close(fd);
}

struct http_server *http_server_open(unsigned short port, unsigned int max_clients,
                                     struct frame_store *store)
{
    struct http_server *srv;
    struct sockaddr_in addr;
    int opt = 1;

    srv = calloc(1, sizeof(*srv));
    if (srv == NULL) {
        syslog(LOG_ERR, "Out of memory");
        return NULL;
    }
    srv->max_clients = max_clients;
    srv->store = store;
    SLIST_INIT(&srv->clients);

    srv->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (srv->listen_fd == -1) {
        syslog(LOG_ERR, "socket failed: %m");
        free(srv);
        return NULL;
    }
    if (setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        syslog(LOG_ERR, "setsockopt SO_REUSEADDR failed: %m");
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        syslog(LOG_ERR, "bind to port %u failed: %m", port);
        close(srv->listen_fd);
        free(srv);
        return NULL;
    }
    if (listen(srv->listen_fd, LISTEN_BACKLOG) == -1) {
        syslog(LOG_ERR, "listen failed: %m");
        close(srv->listen_fd);
        free(srv);
        return NULL;
    }

    syslog(LOG_INFO, "HTTP server listening on port %u, up to %u clients", port, max_clients);
    return srv;
}

int http_server_listen_fd(const struct http_server *srv)
{
    return srv->listen_fd;
}

void http_server_accept(struct http_server *srv)
{
    /* Free slots held by clients that have already finished */
    http_server_reap(srv);

    for (;;) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        char addr_str[INET_ADDRSTRLEN];
        struct http_client *c;
        int fd;
        int rc;

        /* Client sockets are blocking; the listening socket isn't, so this loop ends */
        fd = accept4(srv->listen_fd, (struct sockaddr *)&addr, &addr_len, SOCK_CLOEXEC);
        if (fd == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                syslog(LOG_ERR, "accept failed: %m");
            }
            return;
        }
        inet_ntop(AF_INET, &addr.sin_addr, addr_str, sizeof(addr_str));

        if (srv->num_clients >= srv->max_clients) {
            syslog(LOG_INFO, "Rejected %s with 503: %u clients connected", addr_str,
                   srv->num_clients);
            reject_busy(fd, addr_str);
            continue;
        }

        c = calloc(1, sizeof(*c));
        if (c == NULL) {
            syslog(LOG_ERR, "Out of memory for client %s", addr_str);
            reject_busy(fd, addr_str);
            continue;
        }
        c->fd = fd;
        c->store = srv->store;
        atomic_init(&c->thread_complete, 0);
        memcpy(c->addr, addr_str, sizeof(addr_str));

        rc = thread_create_signals_blocked(&c->thread, client_thread, c);
        if (rc != 0) {
            syslog(LOG_ERR, "pthread_create for client %s failed: %s", addr_str, strerror(rc));
            reject_busy(fd, addr_str);
            free(c);
            continue;
        }
        SLIST_INSERT_HEAD(&srv->clients, c, entries);
        srv->num_clients++;
        syslog(LOG_INFO, "Accepted connection from %s (%u clients)", addr_str, srv->num_clients);
    }
}

static void client_join(struct http_server *srv, struct http_client *c)
{
    pthread_join(c->thread, NULL);
    close(c->fd);
    SLIST_REMOVE(&srv->clients, c, http_client, entries);
    srv->num_clients--;
    free(c);
}

void http_server_reap(struct http_server *srv)
{
    struct http_client *c = SLIST_FIRST(&srv->clients);

    while (c != NULL) {
        struct http_client *next = SLIST_NEXT(c, entries);

        if (atomic_load(&c->thread_complete)) {
            client_join(srv, c);
        }
        c = next;
    }
}

void http_server_close(struct http_server *srv)
{
    struct http_client *c;

    if (srv == NULL) {
        return;
    }

    /* Unblock any recv() or send() in progress; the threads then see an error and finish */
    SLIST_FOREACH(c, &srv->clients, entries) {
        shutdown(c->fd, SHUT_RDWR);
    }
    while (!SLIST_EMPTY(&srv->clients)) {
        client_join(srv, SLIST_FIRST(&srv->clients));
    }

    close(srv->listen_fd);
    free(srv);
}
