/**
 * @file http_server.h
 * @brief HTTP server for the live MJPEG stream, with one thread per client.
 *
 * Paths: "/" (HTML page showing the stream), "/stream" (multipart/x-mixed-replace MJPEG),
 * "/snapshot.jpg" (newest frame). The caller runs the accept loop: it waits for the listening
 * socket to become readable, then calls http_server_accept(). Connections beyond the client
 * limit receive 503 Service Unavailable.
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "frame_store.h"

struct http_server;

/**
 * Create the non-blocking listening socket on all interfaces.
 * @param store frame store that client threads read frames from
 * @return the server, or NULL on error (logged to syslog)
 */
struct http_server *http_server_open(unsigned short port, unsigned int max_clients,
                                     struct frame_store *store);

/** Listening socket, readable (POLLIN) when connections are waiting. */
int http_server_listen_fd(const struct http_server *srv);

/** Join finished client threads, then accept every waiting connection and start its thread. */
void http_server_accept(struct http_server *srv);

/** Join client threads that have finished. Call regularly from the accept loop. */
void http_server_reap(struct http_server *srv);

/**
 * Disconnect all clients, join their threads, close the listening socket and free the server.
 * Call frame_store_shutdown() first so streaming clients stop waiting for frames.
 */
void http_server_close(struct http_server *srv);

#endif /* HTTP_SERVER_H */
