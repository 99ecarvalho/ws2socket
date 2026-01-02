/**
 * @file websocket.c
 * @brief WebSocket Protocol Implementation
 * @author WebSocket to TCP Proxy Project
 * @version 0.1.0
 * 
 * RFC 6455 WebSocket Protocol implementation
 * License: LGPL v3
 */

#include "websocket.h"
#include "utils.h"
#include "logging.h"
#include <openssl/sha.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

/** RFC 6455 GUID constant */
#define WEBSOCKET_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/**
 * @brief Create a new WebSocket connection object
 */
websocket_t *websocket_create(void)
{
    websocket_t *ws;

    ws = (websocket_t *)calloc(1, sizeof(websocket_t));
    if (!ws) {
        return NULL;
    }

    ws->state = WS_STATE_NEW;
    ws->sock_fd = -1;
    ws->ssl = NULL;
    ws->close_code = 0;
    ws->close_reason = NULL;
    ws->headers = NULL;
    ws->protocol = NULL;

    if (buffer_init(&ws->recv_buf, MAX_FRAME_SIZE) != WS_SUCCESS ||
        buffer_init(&ws->send_buf, MAX_FRAME_SIZE) != WS_SUCCESS) {
        websocket_destroy(ws);
        return NULL;
    }

    return ws;
}

/**
 * @brief Destroy a WebSocket connection object
 */
void websocket_destroy(websocket_t *ws)
{
    if (!ws) {
        return;
    }

    buffer_destroy(&ws->recv_buf);
    buffer_destroy(&ws->send_buf);

    if (ws->partial_msg) {
        free(ws->partial_msg);
    }

    if (ws->close_reason) {
        free(ws->close_reason);
    }

    if (ws->headers) {
        free(ws->headers);
    }

    if (ws->protocol) {
        free(ws->protocol);
    }

    free(ws);
}

/**
 * @brief Initialize WebSocket with socket and SSL
 */
int websocket_init(websocket_t *ws, int sock_fd, SSL *ssl)
{
    if (!ws || sock_fd < 0) {
        return WS_EINVAL;
    }

    ws->sock_fd = sock_fd;
    ws->ssl = ssl;
    ws->state = WS_STATE_CONNECTING;

    return WS_SUCCESS;
}

/**
 * @brief Accept WebSocket connection from client
 */
int websocket_accept(websocket_t *ws, const char **http_headers,
                    int num_headers)
{
    /* TODO: Implement full WebSocket handshake according to RFC 6455 */
    if (!ws || !http_headers || num_headers == 0) {
        return WS_EINVAL;
    }

    /* 1. Verify Upgrade header */
    /* 2. Verify Connection header */
    /* 3. Extract Sec-WebSocket-Key */
    /* 4. Generate Sec-WebSocket-Accept response */
    /* 5. Send HTTP 101 response */
    /* 6. Set state to OPEN */

    log_debug("WebSocket accept: TODO - implement full handshake");
    ws->state = WS_STATE_OPEN;

    return WS_SUCCESS;
}

/**
 * @brief Connect WebSocket to server (client-side)
 */
int websocket_connect(websocket_t *ws, const char *host, uint16_t port,
                     const char *path, const char **protocols,
                     int num_protocols)
{
    /* TODO: Implement client-side WebSocket handshake */
    (void)ws;
    (void)host;
    (void)port;
    (void)path;
    (void)protocols;
    (void)num_protocols;

    log_debug("WebSocket connect: TODO - implement client handshake");
    return WS_SUCCESS;
}

/**
 * @brief Send data as WebSocket frame
 */
ssize_t websocket_send(websocket_t *ws, const uint8_t *data,
                      size_t data_len, uint8_t opcode)
{
    /* TODO: Implement frame encoding and masking */
    (void)ws;
    (void)data;
    (void)data_len;
    (void)opcode;

    log_debug("WebSocket send: TODO - implement frame encoding");
    return 0;
}

/**
 * @brief Receive data from WebSocket frame
 */
ssize_t websocket_recv(websocket_t *ws, uint8_t *data, size_t data_len)
{
    /* TODO: Implement frame reception and decoding */
    (void)ws;
    (void)data;
    (void)data_len;

    log_debug("WebSocket recv: TODO - implement frame decoding");
    return 0;
}

/**
 * @brief Send WebSocket ping frame
 */
int websocket_ping(websocket_t *ws, const uint8_t *data, size_t data_len)
{
    (void)ws;
    (void)data;
    (void)data_len;

    log_debug("WebSocket ping: TODO - implement");
    return WS_SUCCESS;
}

/**
 * @brief Send WebSocket pong frame
 */
int websocket_pong(websocket_t *ws, const uint8_t *data, size_t data_len)
{
    (void)ws;
    (void)data;
    (void)data_len;

    log_debug("WebSocket pong: TODO - implement");
    return WS_SUCCESS;
}

/**
 * @brief Close WebSocket connection gracefully
 */
int websocket_close(websocket_t *ws, uint16_t code, const char *reason)
{
    (void)ws;
    (void)code;
    (void)reason;

    log_debug("WebSocket close: TODO - implement");
    return WS_SUCCESS;
}

/**
 * @brief Check if connection has pending data
 */
int websocket_pending(const websocket_t *ws)
{
    if (!ws) {
        return 0;
    }

    return !buffer_is_empty(&ws->recv_buf);
}

/**
 * @brief Get WebSocket state
 */
int websocket_get_state(const websocket_t *ws)
{
    if (!ws) {
        return WS_STATE_NEW;
    }

    return ws->state;
}

/**
 * @brief Get peer name (remote address)
 */
int websocket_getpeername(const websocket_t *ws, struct sockaddr *addr_out,
                         socklen_t *addr_len)
{
    if (!ws || ws->sock_fd < 0 || !addr_out || !addr_len) {
        return WS_EINVAL;
    }

    if (getpeername(ws->sock_fd, addr_out, addr_len) < 0) {
        log_error("getpeername() failed: %s", strerror(errno));
        return WS_ESOCKET;
    }

    return WS_SUCCESS;
}

/**
 * @brief Get socket name (local address)
 */
int websocket_getsockname(const websocket_t *ws, struct sockaddr *addr_out,
                         socklen_t *addr_len)
{
    if (!ws || ws->sock_fd < 0 || !addr_out || !addr_len) {
        return WS_EINVAL;
    }

    if (getsockname(ws->sock_fd, addr_out, addr_len) < 0) {
        log_error("getsockname() failed: %s", strerror(errno));
        return WS_ESOCKET;
    }

    return WS_SUCCESS;
}

/**
 * @brief Set socket timeout
 */
int websocket_set_timeout(websocket_t *ws, int timeout_sec)
{
    if (!ws || ws->sock_fd < 0) {
        return WS_EINVAL;
    }

    return socket_set_timeout(ws->sock_fd, timeout_sec);
}

/**
 * @brief Set non-blocking mode
 */
int websocket_set_blocking(websocket_t *ws, int blocking)
{
    if (!ws || ws->sock_fd < 0) {
        return WS_EINVAL;
    }

    if (!blocking) {
        return socket_set_nonblocking(ws->sock_fd);
    }

    /* TODO: Set blocking mode */
    return WS_SUCCESS;
}
