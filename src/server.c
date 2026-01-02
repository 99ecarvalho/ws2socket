/**
 * @file server.c
 * @brief HTTP/WebSocket Server Implementation
 * @author WebSocket to TCP Proxy Project
 * @version 0.1.0
 * 
 * License: LGPL v3
 */

#include "server.h"
#include "logging.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>

/**
 * @brief Create new WebSocket server instance
 */
ws_server_t *server_create(void)
{
    ws_server_t *server;

    server = (ws_server_t *)calloc(1, sizeof(ws_server_t));
    if (!server) {
        return NULL;
    }

    server->listen_fd = -1;
    server->ssl_ctx = NULL;
    server->running = 0;
    server->num_connections = 0;
    server->max_reached = 0;

    return server;
}

/**
 * @brief Destroy WebSocket server instance
 */
void server_destroy(ws_server_t *server)
{
    if (!server) {
        return;
    }

    if (server->listen_fd >= 0) {
        close(server->listen_fd);
    }

    if (server->ssl_ctx) {
        SSL_CTX_free(server->ssl_ctx);
    }

    free(server);
}

/**
 * @brief Initialize server with configuration
 */
int server_init(ws_server_t *server, const server_config_t *config)
{
    if (!server || !config) {
        return WS_EINVAL;
    }

    memcpy(&server->config, config, sizeof(server_config_t));

    /* Initialize SSL if needed */
    if (config->use_ssl) {
        /* TODO: Initialize SSL context */
        log_debug("SSL configuration: cert=%s, key=%s", 
                 config->cert_file, config->key_file);
    }

    log_info("Server initialized: %s:%u (SSL=%d)", config->listen_host,
            config->listen_port, config->use_ssl);

    return WS_SUCCESS;
}

/**
 * @brief Start server listening
 */
int server_listen(ws_server_t *server)
{
    int ret;

    if (!server) {
        return WS_EINVAL;
    }

    /* Create socket */
    server->listen_fd = socket_create_tcp(1);
    if (server->listen_fd < 0) {
        log_error("Failed to create listen socket");
        return WS_ESOCKET;
    }

    /* Bind */
    ret = socket_bind(server->listen_fd, server->config.listen_host,
                     server->config.listen_port);
    if (ret != WS_SUCCESS) {
        log_error("Failed to bind to %s:%u", server->config.listen_host,
                 server->config.listen_port);
        close(server->listen_fd);
        server->listen_fd = -1;
        return ret;
    }

    /* Listen */
    ret = socket_listen(server->listen_fd, server->config.backlog);
    if (ret != WS_SUCCESS) {
        log_error("Failed to listen on socket");
        close(server->listen_fd);
        server->listen_fd = -1;
        return ret;
    }

    server->running = 1;
    log_info("Server listening on %s:%u", server->config.listen_host,
            server->config.listen_port);

    return WS_SUCCESS;
}

/**
 * @brief Accept next client connection
 */
int server_accept_client(ws_server_t *server, int *client_fd,
                        struct sockaddr_storage *client_addr,
                        socklen_t *client_addr_len, SSL **ssl_ptr)
{
    int ret;

    if (!server || !client_fd || !client_addr || !client_addr_len) {
        return WS_EINVAL;
    }

    ret = socket_accept(server->listen_fd, client_addr, client_addr_len);
    if (ret < 0) {
        return ret;
    }

    *client_fd = ret;

    /* TODO: Handle SSL accept */
    if (ssl_ptr) {
        *ssl_ptr = NULL;
    }

    server->num_connections++;
    if (server->num_connections >= server->config.max_connections) {
        server->max_reached = 1;
    }

    return WS_SUCCESS;
}

/**
 * @brief Receive HTTP request from client
 */
int server_recv_request(int client_fd, http_request_t *req)
{
    if (!req) {
        return WS_EINVAL;
    }
    
    // Declare function from http_server.c
    extern int http_parse_request(const char *request_data, size_t request_len,
                                 http_request_t *request_out);
    
    // Read HTTP request
    char buffer[16384];
    ssize_t total = 0;
    ssize_t n;
    
    // Read until we get \r\n\r\n (end of headers)
    while (total < (ssize_t)sizeof(buffer) - 1) {
        n = socket_recv(client_fd, (uint8_t *)buffer + total, sizeof(buffer) - total - 1, 0);
        if (n <= 0) {
            if (n == 0) {
                log_debug("Client closed connection during request");
            } else {
                log_error("Failed to read HTTP request: %s", strerror(errno));
            }
            return WS_ESOCKET;
        }
        
        total += n;
        buffer[total] = '\0';
        
        // Check if we have complete headers
        if (strstr(buffer, "\r\n\r\n")) {
            break;
        }
    }
    
    if (total == 0) {
        return WS_EPROTO;
    }
    
    // Parse the request
    if (http_parse_request(buffer, total, req) < 0) {
        log_error("Failed to parse HTTP request");
        return WS_EPROTO;
    }
    
    log_debug("HTTP Request: %s %s %s", req->method, req->path, req->version);
    return WS_SUCCESS;
}

/**
 * @brief Free HTTP request resources
 */
void http_request_free(http_request_t *req)
{
    if (!req) {
        return;
    }

    if (req->body) {
        free(req->body);
    }

    memset(req, 0, sizeof(http_request_t));
}

/**
 * @brief Send HTTP response
 */
int server_send_response(int client_fd, int code, const char *reason,
                        const char **headers, int num_headers,
                        const uint8_t *body, size_t body_len)
{
    (void)client_fd;
    (void)code;
    (void)reason;
    (void)headers;
    (void)num_headers;
    (void)body;
    (void)body_len;

    /* TODO: Implement HTTP response sending */
    log_debug("server_send_response: TODO - implement");

    return WS_SUCCESS;
}

/**
 * @brief Send HTTP error response
 */
int server_send_error(int client_fd, int code, const char *message)
{
    (void)client_fd;
    (void)code;
    (void)message;

    /* TODO: Implement error response */
    log_debug("server_send_error: TODO - implement");

    return WS_SUCCESS;
}

/**
 * @brief Get HTTP header value
 */
const char *http_get_header(const http_request_t *req,
                           const char *header_name)
{
    (void)req;
    (void)header_name;

    /* TODO: Implement header lookup */
    return NULL;
}

/**
 * @brief Main server loop
 */
int server_run(ws_server_t *server, client_handler_t handler)
{
    int client_fd;
    struct sockaddr_storage client_addr;
    socklen_t client_addr_len;
    SSL *ssl;

    if (!server || !handler) {
        return WS_EINVAL;
    }

    while (server->running) {
        /* Accept incoming connection */
        int ret = server_accept_client(server, &client_fd, &client_addr,
                                       &client_addr_len, &ssl);
        if (ret != WS_SUCCESS) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        /* Handle client */
        handler(server, client_fd, &client_addr, ssl);

        server->num_connections--;
        close(client_fd);
    }

    return WS_SUCCESS;
}

/**
 * @brief Stop server
 */
void server_stop(ws_server_t *server)
{
    if (!server) {
        return;
    }

    server->running = 0;
}

/**
 * @brief Get server statistics
 */
int server_get_stats(ws_server_t *server, int *num_connections)
{
    if (!server || !num_connections) {
        return WS_EINVAL;
    }

    *num_connections = server->num_connections;

    return WS_SUCCESS;
}
