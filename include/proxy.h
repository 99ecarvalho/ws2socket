/**
 * @file proxy.h
 * @brief TCP Proxy functionality
 * @author Eduardo Correia <ecorreia@apliant.com.br>
 * 
 * Handles bidirectional proxying between WebSocket connections and
 * TCP sockets, including data forwarding and connection management.
 * 
 * @copyright Copyright (c) 2026 Eduardo Correia <ecorreia@apliant.com.br>
 *
 * This file is part of ws2socket. It is free software, licensed under the
 * GNU Lesser General Public License v3.0 or later. See COPYING.LESSER
 * and COPYING for details.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef WS2SOCKET_PROXY_H
#define WS2SOCKET_PROXY_H

#include "common.h"
#include "websocket.h"
#include <pthread.h>

/**
 * @struct proxy_client
 * @brief Proxy client connection
 * 
 * Represents a client connection being proxied between WebSocket
 * and TCP socket.
 */
typedef struct {
    /** WebSocket connection from client */
    websocket_t *ws;
    /** TCP socket to target server */
    int target_fd;
    /** Client connection ID */
    uint32_t client_id;
    /** Source address */
    struct sockaddr_storage src_addr;
    /** Source address length */
    socklen_t src_addr_len;
    /** Target hostname */
    char target_host[256];
    /** Target port */
    uint16_t target_port;
    /** Send buffer (client to server) */
    ws_buffer_t send_buf;
    /** Receive buffer (server to client) */
    ws_buffer_t recv_buf;
    /** Connection timestamp */
    time_t connect_time;
    /** Bytes sent to target (uncompressed) */
    uint64_t bytes_sent;
    /** Bytes received from target (uncompressed) */
    uint64_t bytes_received;
    /** Bytes sent compressed (WS wire) */
    uint64_t bytes_sent_compressed;
    /** Bytes received compressed (WS wire) */
    uint64_t bytes_received_compressed;
    /** Flag: connection active */
    uint8_t active;
    /** Mutex for thread safety */
    pthread_mutex_t lock;
} proxy_client_t;

/**
 * @struct proxy_config
 * @brief Proxy configuration
 * 
 * Configuration for the TCP proxy.
 */
typedef struct {
    /** Maximum concurrent connections */
    int max_connections;
    /** Buffer size per client */
    size_t buffer_size;
    /** Socket timeout (seconds) */
    int socket_timeout;
    /** Use TCP_NODELAY flag */
    int tcp_nodelay;
    /** Enable traffic logging */
    int log_traffic;
    /** Enable heartbeat/keepalive */
    int heartbeat_enabled;
    /** Heartbeat interval (ms) */
    int heartbeat_interval;
} proxy_config_t;

/**
 * @brief Initialize proxy system
 * 
 * Sets up the proxy with the given configuration.
 * 
 * @param config Proxy configuration
 * @return WS_SUCCESS on success, error code otherwise
 */
int proxy_init(const proxy_config_t *config);

/**
 * @brief Shutdown proxy system
 * 
 * Closes all active connections and cleans up resources.
 * 
 * @return WS_SUCCESS on success, error code otherwise
 */
int proxy_shutdown(void);

/**
 * @brief Create new proxy client
 * 
 * Allocates and initializes a new proxy client structure.
 * 
 * @return Pointer to new proxy_client_t, NULL on failure
 * 
 * @note Must call proxy_client_destroy() to free resources
 */
proxy_client_t *proxy_client_create(void);

/**
 * @brief Destroy proxy client
 * 
 * Closes sockets and frees all resources associated with a proxy client.
 * 
 * @param client Proxy client to destroy
 */
void proxy_client_destroy(proxy_client_t *client);

/**
 * @brief Connect proxy to target server
 * 
 * Establishes a TCP connection to the target server from the proxy client.
 * 
 * @param client Proxy client
 * @param target_host Target hostname or IP
 * @param target_port Target port
 * @param use_ssl Use SSL/TLS for target connection
 * @return WS_SUCCESS on success, error code otherwise
 */
int proxy_connect_target(proxy_client_t *client, const char *target_host,
                        uint16_t target_port, int use_ssl);

/**
 * @brief Start proxying between WebSocket and TCP socket
 * 
 * Main proxy loop that bidirectionally forwards data between the
 * WebSocket client and target TCP socket. Runs until connection closes.
 * 
 * @param client Proxy client
 * @return WS_SUCCESS on normal close, error code on failure
 * 
 * @note This function blocks until the connection is closed
 */
int proxy_forward(proxy_client_t *client);

/**
 * @brief Forward data from WebSocket to TCP socket
 * 
 * Reads data from WebSocket and sends to target TCP socket.
 * 
 * @param client Proxy client
 * @return Number of bytes forwarded, 0 if WS closed, negative on error
 */
ssize_t proxy_forward_ws_to_tcp(proxy_client_t *client);

/**
 * @brief Forward data from TCP socket to WebSocket
 * 
 * Reads data from target TCP socket and sends to WebSocket client.
 * 
 * @param client Proxy client
 * @return Number of bytes forwarded, 0 if TCP closed, negative on error
 */
ssize_t proxy_forward_tcp_to_ws(proxy_client_t *client);

/**
 * @brief Gracefully close proxy connection
 * 
 * Closes both WebSocket and TCP connections cleanly.
 * 
 * @param client Proxy client
 * @param code WebSocket close code
 * @param reason Close reason message
 * @return WS_SUCCESS on success, error code otherwise
 */
int proxy_close(proxy_client_t *client, uint16_t code, const char *reason);

/**
 * @brief Get proxy client statistics
 * 
 * Retrieves current statistics for a proxy client.
 * 
 * @param client Proxy client
 * @param bytes_sent Pointer to store bytes sent (can be NULL)
 * @param bytes_received Pointer to store bytes received (can be NULL)
 * @param active Pointer to store active status (can be NULL)
 * @return WS_SUCCESS on success, error code otherwise
 */
int proxy_client_stats(const proxy_client_t *client, uint64_t *bytes_sent,
                      uint64_t *bytes_received, int *active);

/**
 * @brief Get current proxy statistics
 * 
 * @param num_active Pointer to store number of active connections
 * @return WS_SUCCESS on success, error code otherwise
 */
int proxy_get_stats(int *num_active);

/**
 * @brief Log proxy traffic for debugging
 * 
 * @param client Proxy client
 * @param direction Direction indicator (">", "<", "}"), ".", or combined)
 * @return WS_SUCCESS on success
 */
int proxy_log_traffic(proxy_client_t *client, const char *direction);

#endif /* WS2SOCKET_PROXY_H */
