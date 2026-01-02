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
    if (!ws || !http_headers || num_headers == 0) {
        return WS_EINVAL;
    }

    // Extract Sec-WebSocket-Key from headers
    const char *ws_key = NULL;
    for (int i = 0; i < num_headers; i++) {
        if (strncasecmp(http_headers[i], "Sec-WebSocket-Key:", 18) == 0) {
            ws_key = http_headers[i] + 18;
            // Skip whitespace
            while (*ws_key == ' ' || *ws_key == '\t') ws_key++;
            break;
        }
    }
    
    if (!ws_key) {
        log_error("Missing Sec-WebSocket-Key header");
        return WS_EPROTO;
    }
    
    // Perform handshake (implemented in websocket_impl.c)
    extern int websocket_do_handshake(websocket_t *ws, const char *sec_key);
    return websocket_do_handshake(ws, ws_key);
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
    extern ssize_t websocket_send_frame(websocket_t *ws, const uint8_t *data,
                                       size_t data_len, uint8_t opcode, int fin);
    return websocket_send_frame(ws, data, data_len, opcode, 1);
}

/**
 * @brief Receive data from WebSocket frame
 */
ssize_t websocket_recv(websocket_t *ws, uint8_t *data, size_t data_len)
{
    extern ssize_t websocket_recv_frame(websocket_t *ws, uint8_t *data_out,
                                       size_t data_len, uint8_t *opcode_out);
    return websocket_recv_frame(ws, data, data_len, NULL);
}

/**
 * @brief Send WebSocket ping frame
 */
int websocket_ping(websocket_t *ws, const uint8_t *data, size_t data_len)
{
    extern ssize_t websocket_send_frame(websocket_t *ws, const uint8_t *data,
                                       size_t data_len, uint8_t opcode, int fin);
    if (websocket_send_frame(ws, data, data_len, WS_OPCODE_PING, 1) < 0) {
        return WS_ESOCKET;
    }
    return WS_SUCCESS;
}

/**
 * @brief Send WebSocket pong frame
 */
int websocket_pong(websocket_t *ws, const uint8_t *data, size_t data_len)
{
    extern ssize_t websocket_send_frame(websocket_t *ws, const uint8_t *data,
                                       size_t data_len, uint8_t opcode, int fin);
    if (websocket_send_frame(ws, data, data_len, WS_OPCODE_PONG, 1) < 0) {
        return WS_ESOCKET;
    }
    return WS_SUCCESS;
}

/**
 * @brief Close WebSocket connection gracefully
 */
int websocket_close(websocket_t *ws, uint16_t code, const char *reason)
{
    if (!ws) {
        return WS_EINVAL;
    }
    
    extern ssize_t websocket_send_frame(websocket_t *ws, const uint8_t *data,
                                       size_t data_len, uint8_t opcode, int fin);
    
    uint8_t close_frame[125];
    size_t frame_len = 0;
    
    if (code > 0) {
        close_frame[frame_len++] = (code >> 8) & 0xFF;
        close_frame[frame_len++] = code & 0xFF;
        
        if (reason) {
            size_t reason_len = strlen(reason);
            if (frame_len + reason_len <= sizeof(close_frame)) {
                memcpy(close_frame + frame_len, reason, reason_len);
                frame_len += reason_len;
            }
        }
    }
    
    websocket_send_frame(ws, close_frame, frame_len, WS_OPCODE_CLOSE, 1);
    ws->state = WS_STATE_CLOSING;
    
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
