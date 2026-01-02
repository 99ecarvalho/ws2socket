/**
 * @file server.h
 * @brief HTTP/WebSocket Server
 * @author WebSocket to TCP Proxy Project
 * @version 0.1.0
 * 
 * Implements an HTTP server with WebSocket upgrade support.
 * Handles client connections, SSL/TLS, and request dispatching.
 * 
 * License: LGPL v3
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

/**
 * @struct http_request
 * @brief HTTP request information
 * 
 * Parsed HTTP request data from client.
 */
typedef struct {
    /** HTTP method (GET, POST, HEAD, etc) */
    char method[16];
    /** Request path */
    char path[512];
    /** HTTP version ("HTTP/1.0" or "HTTP/1.1") */
    char version[16];
    /** Raw header lines */
    char **headers;
    /** Number of header lines */
    int num_headers;
    /** Request body (if any) */
    uint8_t *body;
    /** Body length */
    size_t body_len;
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
 * @param req Pointer to http_request_t to fill
 * @return WS_SUCCESS on success, error code otherwise
 * 
 * @note Memory for request must be freed with http_request_free()
 */
int server_recv_request(int client_fd, http_request_t *req);

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
