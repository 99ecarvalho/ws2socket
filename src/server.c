/**
 * @file server.c
 * @brief HTTP/WebSocket Server Implementation
 * @author Eduardo Correia <ecorreia@apliant.com.br>
 * 
 * @copyright Copyright (c) 2026 Eduardo Correia <ecorreia@apliant.com.br>
 *
 * This file is part of ws2socket. It is free software, licensed under the
 * GNU Lesser General Public License v3.0 or later. See COPYING.LESSER
 * and COPYING for details.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "server.h"
#include "logging.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/wait.h>
#include <signal.h>

/** Number of live child processes (one per client connection) */
static volatile sig_atomic_t g_active_children = 0;

/**
 * @brief Signal handler for SIGCHLD to reap zombie processes
 */
static void sigchld_handler(int sig)
{
    int saved_errno = errno;
    (void)sig;
    /* Reap all terminated child processes */
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        if (g_active_children > 0) {
            g_active_children--;
        }
    }
    errno = saved_errno;
}

/**
 * @brief Perform the TLS handshake on a freshly accepted connection
 *
 * Runs in the child process so a slow or silent client cannot stall the
 * accept loop. The socket's receive/send timeouts bound the handshake.
 *
 * @return TLS session, or NULL if the handshake failed
 */
static SSL *server_tls_handshake(ws_server_t *server, int client_fd)
{
    SSL *ssl = SSL_new(server->ssl_ctx);
    if (!ssl) {
        log_error("Failed to create SSL structure");
        return NULL;
    }

    SSL_set_fd(ssl, client_fd);

    int ssl_ret = SSL_accept(ssl);
    if (ssl_ret <= 0) {
        log_warn("TLS handshake failed: error %d", SSL_get_error(ssl, ssl_ret));
        SSL_free(ssl);
        return NULL;
    }

    log_debug("TLS connection established (%s)", SSL_get_version(ssl));
    return ssl;
}

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
        log_info("Initializing SSL/TLS context");
        
        /* Initialize OpenSSL library */
        SSL_load_error_strings();
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        
        /* Create SSL context */
        const SSL_METHOD *method = TLS_server_method();
        server->ssl_ctx = SSL_CTX_new(method);
        if (!server->ssl_ctx) {
            log_error("Failed to create SSL context");
            return WS_ESSL;
        }
        
        /* Set SSL options for security */
        SSL_CTX_set_options(server->ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | 
                           SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);
        SSL_CTX_set_mode(server->ssl_ctx, SSL_MODE_AUTO_RETRY);
        
        /* Load certificate file */
        if (strlen(config->cert_file) > 0) {
            if (SSL_CTX_use_certificate_file(server->ssl_ctx, config->cert_file, 
                                            SSL_FILETYPE_PEM) <= 0) {
                log_error("Failed to load certificate from %s", config->cert_file);
                SSL_CTX_free(server->ssl_ctx);
                server->ssl_ctx = NULL;
                return WS_ESSL;
            }
            log_info("Loaded SSL certificate: %s", config->cert_file);
        } else {
            log_error("SSL enabled but no certificate file specified");
            SSL_CTX_free(server->ssl_ctx);
            server->ssl_ctx = NULL;
            return WS_EINVAL;
        }
        
        /* Load private key file */
        if (strlen(config->key_file) > 0) {
            if (SSL_CTX_use_PrivateKey_file(server->ssl_ctx, config->key_file, 
                                           SSL_FILETYPE_PEM) <= 0) {
                log_error("Failed to load private key from %s", config->key_file);
                SSL_CTX_free(server->ssl_ctx);
                server->ssl_ctx = NULL;
                return WS_ESSL;
            }
            log_info("Loaded SSL private key: %s", config->key_file);
        } else {
            log_error("SSL enabled but no private key file specified");
            SSL_CTX_free(server->ssl_ctx);
            server->ssl_ctx = NULL;
            return WS_EINVAL;
        }
        
        /* Verify private key matches certificate */
        if (!SSL_CTX_check_private_key(server->ssl_ctx)) {
            log_error("Private key does not match certificate");
            SSL_CTX_free(server->ssl_ctx);
            server->ssl_ctx = NULL;
            return WS_ESSL;
        }
        
        log_info("SSL/TLS initialized successfully (TLSv1.2+)");
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

    /* Install SIGCHLD handler to reap zombie processes */
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        log_warn("Failed to install SIGCHLD handler: %s", strerror(errno));
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

    /* The TLS handshake happens in the child process (see server_run) */
    if (ssl_ptr) {
        *ssl_ptr = NULL;
    }

    return WS_SUCCESS;
}

/**
 * @brief Receive HTTP request from client
 */
int server_recv_request(int client_fd, SSL *ssl, http_request_t *req)
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
        n = io_recv(client_fd, ssl, (uint8_t *)buffer + total, sizeof(buffer) - total - 1);
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

    sigset_t chld_set;
    sigemptyset(&chld_set);
    sigaddset(&chld_set, SIGCHLD);

    while (server->running) {
        /* Accept incoming connection */
        client_addr_len = sizeof(client_addr);
        int ret = server_accept_client(server, &client_fd, &client_addr,
                                       &client_addr_len, NULL);
        if (ret != WS_SUCCESS) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            break;
        }

        /* Enforce the connection limit before spending a process on it */
        if (server->config.max_connections > 0 &&
            g_active_children >= server->config.max_connections) {
            log_warn("Connection limit (%d) reached, rejecting client",
                     server->config.max_connections);
            if (!server->ssl_ctx) {
                socket_set_timeout(client_fd, 1);
                http_send_status(client_fd, NULL, 503, "Service Unavailable");
            }
            close(client_fd);
            continue;
        }

        /* Fork to handle client in separate process. SIGCHLD is blocked so
         * the child counter cannot be updated concurrently. */
        sigprocmask(SIG_BLOCK, &chld_set, NULL);
        pid_t pid = fork();
        if (pid > 0) {
            g_active_children++;
            server->num_connections = g_active_children;
        }
        sigprocmask(SIG_UNBLOCK, &chld_set, NULL);
        
        if (pid < 0) {
            /* Fork failed */
            log_error("Failed to fork for client: %s", strerror(errno));
            close(client_fd);
            continue;
        }
        
        if (pid == 0) {
            /* Child process - handle client */
            signal(SIGCHLD, SIG_DFL);
            close(server->listen_fd);  /* Child doesn't need listener socket */

            /* Bound how long a client may stall the TLS handshake, the HTTP
             * request, or a partially sent frame ([server] socket_timeout) */
            if (server->config.socket_timeout > 0) {
                socket_set_timeout(client_fd, server->config.socket_timeout);
            }

            ssl = NULL;
            if (server->ssl_ctx) {
                ssl = server_tls_handshake(server, client_fd);
                if (!ssl) {
                    close(client_fd);
                    exit(1);
                }
            }

            handler(server, client_fd, &client_addr, ssl);

            if (ssl) {
                SSL_shutdown(ssl);
                SSL_free(ssl);
            }
            close(client_fd);
            exit(0);  /* Child exits after handling client */
        }
        
        /* Parent process - close client fd and continue accepting */
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

    *num_connections = g_active_children;

    return WS_SUCCESS;
}
