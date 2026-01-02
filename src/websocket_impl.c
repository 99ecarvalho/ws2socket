/**
 * @file websocket_impl.c
 * @brief Complete WebSocket Protocol Implementation
 * @author ws2socket
 * @version 0.1.0
 * 
 * Full RFC 6455 WebSocket implementation including:
 * - Handshake processing
 * - Frame encoding/decoding
 * - Masking/unmasking
 * - Control frames
 */

#include "websocket.h"
#include "utils.h"
#include "logging.h"
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

/**
 * @brief Parse WebSocket key from HTTP headers and perform handshake
 */
int websocket_do_handshake(websocket_t *ws, const char *sec_key)
{
    if (!ws || !sec_key) {
        return WS_EINVAL;
    }
    
    // RFC 6455: Concatenate key with magic GUID
    static const char *magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char concat[256];
    snprintf(concat, sizeof(concat), "%s%s", sec_key, magic);
    
    // Compute SHA1 hash
    unsigned char hash[20];
    if (sha1_digest((unsigned char *)concat, strlen(concat), hash) != 0) {
        log_error("Failed to compute SHA1 hash for WebSocket handshake");
        return WS_EINTERNAL;
    }
    
    // Base64 encode the hash
    char accept_key[64];
    size_t encoded_len = base64_encode(hash, 20, accept_key, sizeof(accept_key));
    if (encoded_len == 0) {
        log_error("Failed to base64 encode WebSocket accept key");
        return WS_EINTERNAL;
    }
    accept_key[encoded_len] = '\0';
    
    // Send HTTP 101 Switching Protocols response
    char response[512];
    int len = snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n",
        accept_key);
    
    ssize_t sent = socket_send(ws->sock_fd, (uint8_t *)response, len, 0);
    if (sent != len) {
        log_error("Failed to send WebSocket handshake response");
        return WS_ESOCKET;
    }
    
    log_info("WebSocket handshake completed");
    ws->state = WS_STATE_OPEN;
    return WS_SUCCESS;
}

/**
 * @brief Encode and send a WebSocket frame
 */
ssize_t websocket_send_frame(websocket_t *ws, const uint8_t *data,
                             size_t data_len, uint8_t opcode, int fin)
{
    if (!ws || ws->sock_fd < 0) {
        return -1;
    }
    
    if (ws->state != WS_STATE_OPEN) {
        log_warn("Attempt to send on non-open WebSocket");
        return -1;
    }
    
    uint8_t frame[MAX_FRAME_SIZE];
    size_t frame_len = 0;
    
    // Byte 0: FIN + opcode
    frame[frame_len++] = (fin ? 0x80 : 0x00) | (opcode & 0x0F);
    
    // Byte 1: MASK + payload length
    // Server-to-client frames are NOT masked (MASK=0)
    if (data_len < 126) {
        frame[frame_len++] = (uint8_t)data_len;
    } else if (data_len < 65536) {
        frame[frame_len++] = 126;
        frame[frame_len++] = (data_len >> 8) & 0xFF;
        frame[frame_len++] = data_len & 0xFF;
    } else {
        frame[frame_len++] = 127;
        for (int i = 7; i >= 0; i--) {
            frame[frame_len++] = (data_len >> (i * 8)) & 0xFF;
        }
    }
    
    // Copy payload data
    if (data && data_len > 0) {
        if (frame_len + data_len > MAX_FRAME_SIZE) {
            log_error("Frame too large: %zu bytes", frame_len + data_len);
            return -1;
        }
        memcpy(frame + frame_len, data, data_len);
        frame_len += data_len;
    }
    
    // Send the frame
    ssize_t sent = socket_send(ws->sock_fd, frame, frame_len, 0);
    if (sent != (ssize_t)frame_len) {
        log_error("Failed to send WebSocket frame");
        return -1;
    }
    
    return data_len;
}

/**
 * @brief Receive and decode a WebSocket frame
 */
ssize_t websocket_recv_frame(websocket_t *ws, uint8_t *data_out,
                             size_t data_len, uint8_t *opcode_out)
{
    if (!ws || !data_out || ws->sock_fd < 0) {
        return -1;
    }
    
    if (ws->state != WS_STATE_OPEN) {
        return 0;
    }
    
    uint8_t header[14];  // Max header size
    ssize_t n;
    
    // Read first 2 bytes
    n = socket_recv(ws->sock_fd, header, 2, 0);
    if (n <= 0) {
        if (n == 0) {
            ws->state = WS_STATE_CLOSING;
        }
        return n;
    }
    
    if (n < 2) {
        log_error("Incomplete WebSocket frame header");
        return -1;
    }
    
    // Parse header byte 0
    int fin = (header[0] & 0x80) != 0;
    uint8_t opcode = header[0] & 0x0F;
    
    // Parse header byte 1
    int masked = (header[1] & 0x80) != 0;
    uint64_t payload_len = header[1] & 0x7F;
    
    // Read extended payload length if needed
    if (payload_len == 126) {
        n = socket_recv(ws->sock_fd, header + 2, 2, 0);
        if (n != 2) {
            log_error("Failed to read extended payload length");
            return -1;
        }
        payload_len = ((uint64_t)header[2] << 8) | header[3];
    } else if (payload_len == 127) {
        n = socket_recv(ws->sock_fd, header + 2, 8, 0);
        if (n != 8) {
            log_error("Failed to read extended payload length");
            return -1;
        }
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | header[2 + i];
        }
    }
    
    // Read masking key if present
    uint8_t mask[4] = {0};
    if (masked) {
        n = socket_recv(ws->sock_fd, mask, 4, 0);
        if (n != 4) {
            log_error("Failed to read masking key");
            return -1;
        }
    }
    
    // Check payload size
    if (payload_len > data_len) {
        log_error("Payload too large: %lu bytes (buffer: %zu)", 
                 (unsigned long)payload_len, data_len);
        return -1;
    }
    
    // Read payload data
    if (payload_len > 0) {
        size_t total_read = 0;
        while (total_read < payload_len) {
            n = socket_recv(ws->sock_fd, data_out + total_read,
                          payload_len - total_read, 0);
            if (n <= 0) {
                log_error("Failed to read payload data");
                return -1;
            }
            total_read += n;
        }
        
        // Unmask payload if masked
        if (masked) {
            for (size_t i = 0; i < payload_len; i++) {
                data_out[i] ^= mask[i % 4];
            }
        }
    }
    
    if (opcode_out) {
        *opcode_out = opcode;
    }
    
    // Handle control frames
    if (opcode == WS_OPCODE_CLOSE) {
        ws->state = WS_STATE_CLOSING;
        // Send close frame back
        websocket_send_frame(ws, NULL, 0, WS_OPCODE_CLOSE, 1);
        return 0;
    } else if (opcode == WS_OPCODE_PING) {
        // Respond with pong
        websocket_send_frame(ws, data_out, payload_len, WS_OPCODE_PONG, 1);
        return 0;
    }
    
    (void)fin;  // TODO: Handle continuation frames
    
    return payload_len;
}
