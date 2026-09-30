/**
 * @file server.h
 * @brief HTTP/WebSocket Server
 * @author Eduardo Correia <ecorreia@apliant.com.br>
 * 
 * Implements an HTTP server with WebSocket upgrade support.
 * Handles client connections, SSL/TLS, and request dispatching.
 * 
 * @copyright Copyright (c) 2026 Eduardo Correia <ecorreia@apliant.com.br>
 *
 * This file is part of ws2socket. It is free software, licensed under the
 * GNU Lesser General Public License v3.0 or later. See COPYING.LESSER
 * and COPYING for details.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef WS2SOCKET_SERVER_H
#define WS2SOCKET_SERVER_H

#include "common.h"
#include "websocket.h"
#include "proxy.h"
#include <openssl/ssl.h>

/**
 * @struct server_config
 * @brief Server configuration
 * 
 * Configuration for the HTTP/WebSocket server.
 */
typedef struct {
    /** Listen address (hostname or IP) */
    char listen_host[256];
    /** Listen port */
    uint16_t listen_port;
    /** SSL/TLS enabled (wss://) */
    int use_ssl;
    /** SSL certificate file path */
    char cert_file[512];
    /** SSL private key file path */
    char key_file[512];
    /** Only upgrade to WebSocket (no regular HTTP) */
    int only_upgrade;
    /** Enable verbose logging */
    int verbose;
    /** Enable traffic logging */
    int traffic;
    /** Maximum concurrent connections */
    int max_connections;
    /** Socket read/write timeout (seconds) */
    int socket_timeout;
    /** HTTP request timeout (seconds) */
    int http_timeout;
    /** Enable keepalive */
    int keepalive;
    /** Backlog for listen queue */
    int backlog;
} server_config_t;

/** Maximum number of HTTP headers */
#define MAX_HTTP_HEADERS 32

/** Seconds to wait for the next request on a keep-alive connection */
#define HTTP_KEEPALIVE_TIMEOUT 5

/** Requests served on one connection before it is closed */
#define HTTP_KEEPALIVE_MAX_REQUESTS 100

/**
 * @struct http_header
 * @brief HTTP header name-value pair
 */
typedef struct {
    char name[128];
    char value[1024];
} http_header_t;

/**
 * @struct http_request
 * @brief HTTP request information
 * 
 * Parsed HTTP request data from client.
 */
typedef struct {
    /** HTTP method (GET, POST, HEAD, etc) */
    char method[16];
    /** Request target as sent (path and optional query string) */
    char path[2048];
    /** HTTP version ("HTTP/1.0" or "HTTP/1.1") */
    char version[16];
    /** Headers */
    http_header_t headers[MAX_HTTP_HEADERS];
    /** Number of headers */
    int num_headers;
    /** Request body (if any) */
    uint8_t *body;
    /** Body length */
    size_t body_len;
    /** Connection should stay open after the response (HTTP keep-alive) */
    int keep_alive;
    /** Status to answer with when the request could not be read or parsed
     *  (400, 414 or 431), or 0 */
    int error_status;
} http_request_t;

/**
 * @struct ws_server
 * @brief WebSocket server instance
 * 
 * Main server structure managing socket, SSL context, and client connections.
 */
typedef struct {
    /** Server socket file descriptor */
    int listen_fd;
    /** Server configuration */
    server_config_t config;
    /** SSL context (if TLS enabled) */
    SSL_CTX *ssl_ctx;
    /** Server running flag */
    int running;
    /** Number of active connections */
    int num_connections;
    /** Maximum connections reached */
    int max_reached;
    /** Web root directory for static files */
    char web_root[512];
} ws_server_t;

/**
 * @brief Create new WebSocket server instance
 * 
 * Allocates and initializes a new WebSocket server structure.
 * 
 * @return Pointer to new ws_server_t, NULL on failure
 * 
 * @note Must call server_destroy() to free resources
 */
ws_server_t *server_create(void);

/**
 * @brief Destroy WebSocket server instance
 * 
 * Closes server socket and frees all resources.
 * All active client connections must be closed first.
 * 
 * @param server Server instance to destroy
 */
void server_destroy(ws_server_t *server);

/**
 * @brief Initialize server with configuration
 * 
 * Sets up the server with the given configuration, creates socket,
 * and initializes SSL context if needed.
 * 
 * @param server Server instance
 * @param config Pointer to server_config_t
 * @return WS_SUCCESS on success, error code otherwise
 */
int server_init(ws_server_t *server, const server_config_t *config);

/**
 * @brief Start server listening
 * 
 * Binds the server socket to the configured port and starts listening
 * for incoming connections.
 * 
 * @param server Server instance
 * @return WS_SUCCESS on success, error code otherwise
 * 
 * @note Must call server_init() first
 */
int server_listen(ws_server_t *server);

/**
 * @brief Accept next client connection
 * 
 * Waits for and accepts the next incoming client connection.
 * Performs SSL handshake if TLS is configured.
 * 
 * @param server Server instance
 * @param client_fd Pointer to store new client socket FD
 * @param client_addr Pointer to store client address
 * @param client_addr_len Pointer to store address length
 * @param ssl_ptr Pointer to store SSL handle (NULL if not TLS)
 * @return WS_SUCCESS on success, error code otherwise
 */
int server_accept_client(ws_server_t *server, int *client_fd,
                        struct sockaddr_storage *client_addr,
                        socklen_t *client_addr_len, SSL **ssl_ptr);

/**
 * @brief Receive HTTP request from client
 * 
 * Receives and parses an HTTP request from the client socket.
 * 
 * @param client_fd Client socket
 * @param ssl TLS session, or NULL for a plain connection
 * @param req Pointer to http_request_t to fill
 * @return WS_SUCCESS on success, error code otherwise
 * 
 * @note Memory for request must be freed with http_request_free()
 */
int server_recv_request(int client_fd, SSL *ssl, http_request_t *req);

/**
 * @brief Free HTTP request resources
 * 
 * @param req HTTP request to free
 */
void http_request_free(http_request_t *req);

/**
 * @brief Send HTTP response
 * 
 * Sends an HTTP response with headers and optional body.
 * 
 * @param client_fd Client socket
 * @param code HTTP status code (200, 400, 404, etc)
 * @param reason HTTP reason phrase
 * @param headers Array of header strings ("Name: Value")
 * @param num_headers Number of headers
 * @param body Response body (NULL for none)
 * @param body_len Body length (0 if NULL)
 * @return WS_SUCCESS on success, error code otherwise
 */
int server_send_response(int client_fd, int code, const char *reason,
                        const char **headers, int num_headers,
                        const uint8_t *body, size_t body_len);

/**
 * @brief Send HTTP error response
 * 
 * @param client_fd Client socket
 * @param code HTTP error code
 * @param message Error message (will be in response body)
 * @return WS_SUCCESS on success, error code otherwise
 */
int server_send_error(int client_fd, int code, const char *message);

/**
 * @brief Get HTTP header value
 * 
 * @param req HTTP request
 * @param header_name Header name (case-insensitive)
 * @return Header value or NULL if not found
 */
const char *http_get_header(const http_request_t *req,
                           const char *header_name);

/**
 * @brief Check if request is WebSocket upgrade
 * 
 * @param req HTTP request
 * @return 1 if WebSocket upgrade request, 0 otherwise
 */
int http_is_websocket_upgrade(const http_request_t *req);

/**
 * @brief Get a header value from a parsed request
 *
 * @param request HTTP request
 * @param name Header name (case-insensitive)
 * @return Header value or NULL if not present
 */
const char *http_get_header_value(const http_request_t *request, const char *name);

/**
 * @brief Send a short plain-text status response with "Connection: close"
 *
 * @param client_fd Client socket
 * @param ssl TLS session, or NULL for a plain connection
 * @param code HTTP status code
 * @param reason Text used as the plain-text body
 * @return WS_SUCCESS on success, WS_ESOCKET on send failure
 */
int http_send_status(int client_fd, SSL *ssl, int code, const char *reason);

/**
 * @brief Send a complete HTTP response
 *
 * Adds Date, Server, Content-Length and Connection headers. Honours
 * keep-alive and omits the body for HEAD requests.
 *
 * @param client_fd Client socket
 * @param ssl TLS session, or NULL for a plain connection
 * @param req Request being answered (NULL: close the connection)
 * @param code HTTP status code
 * @param extra_headers Additional header lines, each ending in CRLF, or NULL
 * @param content_type Content-Type value, or NULL for none
 * @param body Response body, or NULL
 * @param body_len Body length
 * @return WS_SUCCESS on success, error code otherwise
 */
int http_respond(int client_fd, SSL *ssl, const http_request_t *req, int code,
                 const char *extra_headers, const char *content_type,
                 const void *body, size_t body_len);

/**
 * @brief Reason phrase for an HTTP status code
 */
const char *http_status_reason(int code);

/**
 * @brief MIME type for a file name, from its extension
 */
const char *http_get_mime_type(const char *path);

/**
 * @brief Percent-decode the path part of a request target
 *
 * @param target Request target (the query string and fragment are dropped)
 * @param out Decoded path
 * @param out_size Size of out
 * @return 0 on success, -1 for malformed escapes, encoded NUL or overflow
 */
int http_decode_path(const char *target, char *out, size_t out_size);

/**
 * @brief Serve a static file from the web root
 *
 * Handles GET and HEAD (other methods get 405), percent-decoding, directory
 * redirects and index.html, If-Modified-Since and single byte ranges.
 *
 * @param client_fd Client socket
 * @param ssl TLS session, or NULL for a plain connection
 * @param web_root Directory to serve files from
 * @param req Parsed request
 * @return HTTP status sent, or -1 if the connection failed mid-response
 */
int http_serve_file(int client_fd, SSL *ssl, const char *web_root,
                    const http_request_t *req);

/**
 * @brief Main server loop
 * 
 * Accepts connections and dispatches them. This function runs
 * until stopped by signal or error.
 * 
 * @param server Server instance
 * @param handler Function to handle each client connection
 * @return WS_SUCCESS on shutdown, error code on failure
 * 
 * @note handler will be called for each accepted connection
 *       and should return when client disconnects
 */
typedef int (*client_handler_t)(ws_server_t *server, int client_fd,
                                const struct sockaddr_storage *addr,
                                SSL *ssl);

int server_run(ws_server_t *server, client_handler_t handler);

/**
 * @brief Stop server
 * 
 * Signals the server to stop accepting connections.
 * 
 * @param server Server instance
 */
void server_stop(ws_server_t *server);

/**
 * @brief Get server statistics
 * 
 * @param server Server instance
 * @param num_connections Pointer to store active connections
 * @return WS_SUCCESS on success
 */
int server_get_stats(ws_server_t *server, int *num_connections);

#endif /* WS2SOCKET_SERVER_H */
