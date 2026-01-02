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
#include "logging.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>

/** Global proxy config */
static proxy_config_t g_proxy_config = {0};

/**
 * @brief Initialize proxy system
 */
int proxy_init(const proxy_config_t *config)
{
    if (!config) {
        return WS_EINVAL;
    }

    memcpy(&g_proxy_config, config, sizeof(proxy_config_t));
    log_info("Proxy initialized: max_connections=%d, buffer_size=%zu",
            g_proxy_config.max_connections, g_proxy_config.buffer_size);

    return WS_SUCCESS;
}

/**
 * @brief Shutdown proxy system
 */
int proxy_shutdown(void)
{
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

    /* Create socket */
    client->target_fd = socket_create_tcp(1);
    if (client->target_fd < 0) {
        log_error("Failed to create socket to %s:%u", target_host, target_port);
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
        log_error("Failed to connect to %s:%u", target_host, target_port);
        close(client->target_fd);
        client->target_fd = -1;
        return ret;
    }

    strlcpy(client->target_host, target_host, sizeof(client->target_host));
    client->target_port = target_port;

    log_info("Connected to target %s:%u", target_host, target_port);
    return WS_SUCCESS;
}

/**
 * @brief Start proxying between WebSocket and TCP socket
 */
int proxy_forward(proxy_client_t *client)
{
    int ret;
    fd_set read_set, write_set;
    int max_fd;
    struct timeval tv;

    if (!client || client->target_fd < 0) {
        return WS_EINVAL;
    }

    log_info("Starting proxy for client %u (%s:%u -> %s:%u)", client->client_id,
            "client", 0, client->target_host, client->target_port);

    while (client->active) {
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);

        /* Add WebSocket socket for reading */
        FD_SET(client->ws->sock_fd, &read_set);
        /* Add target socket for reading */
        FD_SET(client->target_fd, &read_set);

        max_fd = (client->ws->sock_fd > client->target_fd) ?
                 client->ws->sock_fd : client->target_fd;

        /* Wait for activity */
        tv.tv_sec = g_proxy_config.socket_timeout;
        tv.tv_usec = 0;

        ret = select(max_fd + 1, &read_set, &write_set, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_error("select() failed: %s", strerror(errno));
            break;
        }

        if (ret == 0) {
            /* Timeout */
            log_debug("Proxy select timeout for client %u", client->client_id);
            continue;
        }

        /* TODO: Handle actual data forwarding */
        /* For now, just break */
        break;
    }

    log_info("Proxy forward ended for client %u", client->client_id);
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

    /* TODO: Implement actual forwarding */
    (void)buffer;
    (void)received;
    (void)sent;

    return 0;
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

    /* TODO: Implement actual forwarding */
    (void)buffer;
    (void)received;
    (void)sent;

    return 0;
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
