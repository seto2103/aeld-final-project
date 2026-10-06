/**
 * @file http_server.h
 * @brief Single-client HTTP server for the live MJPEG stream, driven by the main poll() loop.
 *
 * Paths: "/" (HTML page showing the stream), "/stream" (multipart/x-mixed-replace MJPEG),
 * "/snapshot.jpg" (newest frame). One client is served at a time; other connections receive
 * 503 Service Unavailable. All sockets are non-blocking.
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <poll.h>

#include "frame_store.h"

/* Listening socket plus at most one client */
#define HTTP_SERVER_MAX_POLLFDS 2

struct http_server;

/**
 * Create the listening socket on all interfaces.
 * @return the server, or NULL on error (logged to syslog)
 */
struct http_server *http_server_open(unsigned short port);

/**
 * Fill in the descriptors the server needs to wait on.
 * @return number of entries written, at most HTTP_SERVER_MAX_POLLFDS
 */
int http_server_pollfds(struct http_server *srv, struct pollfd *fds);

/** Handle the poll() results for the entries filled in by http_server_pollfds(). */
void http_server_handle(struct http_server *srv, const struct pollfd *fds, int count,
                        const struct frame_store *store);

/** Send a newly captured frame to a streaming client, skipping it if the client is behind. */
void http_server_new_frame(struct http_server *srv, const struct frame_store *store);

/** Drop a client whose request or stream has stalled. Call at least once a second. */
void http_server_check_timeouts(struct http_server *srv);

/** Disconnect the client, close the listening socket and free the server. */
void http_server_close(struct http_server *srv);

#endif /* HTTP_SERVER_H */
