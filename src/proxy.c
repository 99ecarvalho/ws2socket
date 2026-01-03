/**
 * @file proxy.c
 * @brief TCP Proxy Implementation
 * @author WebSocket to TCP Proxy Project
 * @version 0.1.0
 * 
 * Bidirectional proxying between WebSocket and TCP sockets
 * License: LGPL v3
 */

#include "proxy.h"
#include "conn_pool.h"
#include "logging.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/time.h>

/** Global proxy config */
static proxy_config_t g_proxy_config = {0};

/** Global connection pool */
static conn_pool_t *g_conn_pool = NULL;

/**
 * @brief Initialize proxy system
 */
int proxy_init(const proxy_config_t *config)
{
    if (!config) {
        return WS_EINVAL;
    }

    memcpy(&g_proxy_config, config, sizeof(proxy_config_t));
    
    /* Create connection pool for backend connections */
    g_conn_pool = conn_pool_create(g_proxy_config.max_connections, 300);
    if (!g_conn_pool) {
        log_error("Failed to create connection pool");
        return WS_ENOMEM;
    }
    
    log_info("Proxy initialized: max_connections=%d, buffer_size=%zu",
            g_proxy_config.max_connections, g_proxy_config.buffer_size);

    return WS_SUCCESS;
}

/**
 * @brief Shutdown proxy system
 */
int proxy_shutdown(void)
{
    if (g_conn_pool) {
        conn_pool_destroy(g_conn_pool);
        g_conn_pool = NULL;
    }
    log_info("Proxy shutdown");
    return WS_SUCCESS;
}

/**
 * @brief Create new proxy client
 */
proxy_client_t *proxy_client_create(void)
{
    proxy_client_t *client;

    client = (proxy_client_t *)calloc(1, sizeof(proxy_client_t));
    if (!client) {
        return NULL;
    }

    client->ws = websocket_create();
    if (!client->ws) {
        free(client);
        return NULL;
    }

    client->target_fd = -1;
    client->active = 1;

    if (buffer_init(&client->send_buf, g_proxy_config.buffer_size) != WS_SUCCESS ||
        buffer_init(&client->recv_buf, g_proxy_config.buffer_size) != WS_SUCCESS) {
        proxy_client_destroy(client);
        return NULL;
    }

    if (pthread_mutex_init(&client->lock, NULL) != 0) {
        proxy_client_destroy(client);
        return NULL;
    }

    client->connect_time = time(NULL);

    return client;
}

/**
 * @brief Destroy proxy client
 */
void proxy_client_destroy(proxy_client_t *client)
{
    if (!client) {
        return;
    }

    pthread_mutex_destroy(&client->lock);

    if (client->ws) {
        websocket_destroy(client->ws);
    }

    if (client->target_fd >= 0) {
        close(client->target_fd);
    }

    buffer_destroy(&client->send_buf);
    buffer_destroy(&client->recv_buf);

    free(client);
}

/**
 * @brief Connect proxy to target server
 */
int proxy_connect_target(proxy_client_t *client, const char *target_host,
                        uint16_t target_port, int use_ssl)
{
    int ret;

    if (!client || !target_host || target_port == 0) {
        return WS_EINVAL;
    }

    /* TODO: Implement SSL support */
    (void)use_ssl;

    /* Try to get connection from pool */
    if (g_conn_pool && conn_pool_get(g_conn_pool, target_host, target_port, &client->target_fd) == WS_SUCCESS) {
        log_info("[Client %u] Reused pooled connection to %s:%u (fd=%d)", 
                client->client_id, target_host, target_port, client->target_fd);
    } else {
        /* Create new socket */
        client->target_fd = socket_create_tcp(1);
        if (client->target_fd < 0) {
            log_error("[Client %u] Failed to create socket to %s:%u", 
                     client->client_id, target_host, target_port);
            return WS_ESOCKET;
        }

        /* Set options */
        if (g_proxy_config.tcp_nodelay) {
            socket_set_nodelay(client->target_fd, 1);
        }

        if (g_proxy_config.socket_timeout > 0) {
            socket_set_timeout(client->target_fd, g_proxy_config.socket_timeout);
        }

        /* Connect */
        ret = socket_connect(client->target_fd, target_host, target_port,
                            g_proxy_config.socket_timeout);
        if (ret != WS_SUCCESS) {
            log_error("[Client %u] Failed to connect to %s:%u", 
                     client->client_id, target_host, target_port);
            close(client->target_fd);
            client->target_fd = -1;
            return ret;
        }
        
        log_info("[Client %u] New connection to %s:%u (fd=%d)", 
                client->client_id, target_host, target_port, client->target_fd);
    }

    strlcpy(client->target_host, target_host, sizeof(client->target_host));
    client->target_port = target_port;

    return WS_SUCCESS;
}

/**
 * @brief Start proxying between WebSocket and TCP socket
 */
int proxy_forward(proxy_client_t *client)
{
    int ret;
    fd_set read_set;
    int max_fd;
    struct timeval tv;

    if (!client || client->target_fd < 0) {
        return WS_EINVAL;
    }

    log_info("[Client %u] Starting proxy to %s:%u", client->client_id,
            client->target_host, client->target_port);

    while (client->active) {
        FD_ZERO(&read_set);

        /* Add WebSocket socket for reading */
        FD_SET(client->ws->sock_fd, &read_set);
        /* Add target socket for reading */
        FD_SET(client->target_fd, &read_set);

        max_fd = (client->ws->sock_fd > client->target_fd) ?
                 client->ws->sock_fd : client->target_fd;

        /* Wait for activity */
        tv.tv_sec = g_proxy_config.socket_timeout;
        tv.tv_usec = 0;

        ret = select(max_fd + 1, &read_set, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_error("select() failed: %s", strerror(errno));
            break;
        }

        if (ret == 0) {
            /* Timeout - send ping to keep connection alive */
            continue;
        }

        /* Check WebSocket for data */
        if (FD_ISSET(client->ws->sock_fd, &read_set)) {
            ret = proxy_forward_ws_to_tcp(client);
            if (ret <= 0) {
                log_info("[Client %u] WebSocket closed", client->client_id);
                break;
            }
        }

        /* Check target socket for data */
        if (FD_ISSET(client->target_fd, &read_set)) {
            ret = proxy_forward_tcp_to_ws(client);
            if (ret <= 0) {
                log_info("[Client %u] Target socket closed", client->client_id);
                break;
            }
        }
    }

    /* Calculate connection duration */
    time_t duration = time(NULL) - client->connect_time;
    int hours = duration / 3600;
    int minutes = (duration % 3600) / 60;
    int seconds = duration % 60;
    
    /* Get compressed wire sizes from WebSocket layer */
    uint64_t ws_rx_wire = client->ws->bytes_received_wire;
    uint64_t ws_tx_wire = client->ws->bytes_sent_wire;
    
    /* Calculate compression ratios */
    double rx_ratio = client->bytes_received > 0 ? 
        (double)ws_rx_wire / client->bytes_received * 100.0 : 100.0;
    double tx_ratio = client->bytes_sent > 0 ? 
        (double)ws_tx_wire / client->bytes_sent * 100.0 : 100.0;
    
    /* Log comprehensive disconnect summary (always shown, even in non-verbose mode) */
    log_info("[Client %u] === Connection Closed ===", client->client_id);
    log_info("[Client %u]   Duration: %02d:%02d:%02d", client->client_id, hours, minutes, seconds);
    log_info("[Client %u]   RX: %lu bytes (wire: %lu, compression: %.1f%%)", 
             client->client_id, 
             (unsigned long)client->bytes_received,
             (unsigned long)ws_rx_wire,
             rx_ratio);
    log_info("[Client %u]   TX: %lu bytes (wire: %lu, compression: %.1f%%)", 
             client->client_id,
             (unsigned long)client->bytes_sent,
             (unsigned long)ws_tx_wire,
             tx_ratio);
    log_info("[Client %u]   Total: %lu bytes (wire: %lu, saved: %ld bytes)",
             client->client_id,
             (unsigned long)(client->bytes_received + client->bytes_sent),
             (unsigned long)(ws_rx_wire + ws_tx_wire),
             (long)((client->bytes_received + client->bytes_sent) - (ws_rx_wire + ws_tx_wire)));
    
    return WS_SUCCESS;
}

/**
 * @brief Forward data from WebSocket to TCP socket
 */
ssize_t proxy_forward_ws_to_tcp(proxy_client_t *client)
{
    uint8_t buffer[MAX_FRAME_SIZE];
    ssize_t received, sent;

    if (!client || client->target_fd < 0) {
        return -1;
    }

    // Receive WebSocket frame (this decompresses automatically)
    received = websocket_recv(client->ws, buffer, sizeof(buffer));
    if (received < 0) {
        log_error("Failed to receive WebSocket data");
        return -1;
    }
    
    if (received == 0) {
        // Connection closed
        return 0;
    }

    // Forward to TCP socket
    sent = socket_send(client->target_fd, buffer, received, 0);
    if (sent != received) {
        log_error("Failed to forward data to target");
        return -1;
    }

    // Track uncompressed bytes (after decompression)
    client->bytes_received += received;
    // Note: compressed size is tracked inside websocket_recv via ws->bytes_received_wire
    log_debug("[Client %u] WS->TCP: forwarded %zd bytes", client->client_id, sent);
    
    return sent;
}

/**
 * @brief Forward data from TCP socket to WebSocket
 */
ssize_t proxy_forward_tcp_to_ws(proxy_client_t *client)
{
    uint8_t buffer[MAX_FRAME_SIZE];
    ssize_t received, sent;

    if (!client || client->target_fd < 0) {
        return -1;
    }

    // Receive from TCP socket
    received = socket_recv(client->target_fd, buffer, sizeof(buffer), 0);
    if (received < 0) {
        log_error("Failed to receive TCP data");
        return -1;
    }
    
    if (received == 0) {
        // Connection closed
        return 0;
    }

    // Forward as WebSocket binary frame (this compresses automatically)
    sent = websocket_send(client->ws, buffer, received, WS_OPCODE_BINARY);
    if (sent != received) {
        log_error("Failed to forward data to WebSocket");
        return -1;
    }

    // Track uncompressed bytes (before compression)
    client->bytes_sent += sent;
    // Note: compressed size is tracked inside websocket_send via ws->bytes_sent_wire
    log_debug("[Client %u] TCP->WS: forwarded %zd bytes", client->client_id, sent);
    
    return sent;
}

/**
 * @brief Gracefully close proxy connection
 */
int proxy_close(proxy_client_t *client, uint16_t code, const char *reason)
{
    if (!client) {
        return WS_EINVAL;
    }

    /* TODO: Implement graceful close */
    (void)code;
    (void)reason;

    client->active = 0;

    return WS_SUCCESS;
}

/**
 * @brief Get proxy client statistics
 */
int proxy_client_stats(const proxy_client_t *client, uint64_t *bytes_sent,
                      uint64_t *bytes_received, int *active)
{
    if (!client) {
        return WS_EINVAL;
    }

    pthread_mutex_lock((pthread_mutex_t *)&client->lock);

    if (bytes_sent) {
        *bytes_sent = client->bytes_sent;
    }

    if (bytes_received) {
        *bytes_received = client->bytes_received;
    }

    if (active) {
        *active = client->active;
    }

    pthread_mutex_unlock((pthread_mutex_t *)&client->lock);

    return WS_SUCCESS;
}

/**
 * @brief Get current proxy statistics
 */
int proxy_get_stats(int *num_active)
{
    if (num_active) {
        *num_active = 0;  /* TODO: Implement */
    }

    return WS_SUCCESS;
}

/**
 * @brief Log proxy traffic for debugging
 */
int proxy_log_traffic(proxy_client_t *client, const char *direction)
{
    (void)client;
    (void)direction;

    return WS_SUCCESS;
}
