/**
 * @file websocket_impl.c
 * @brief Complete WebSocket Protocol Implementation
 * @author Eduardo Correia <ecorreia@apliant.com.br>
 * 
 * Full RFC 6455 WebSocket implementation including:
 * - Handshake processing
 * - Frame encoding/decoding
 * - Masking/unmasking
 * - Control frames
 * 
 * @copyright Copyright (c) 2026 Eduardo Correia <ecorreia@apliant.com.br>
 *
 * This file is part of ws2socket. It is free software, licensed under the
 * GNU Lesser General Public License v3.0 or later. See COPYING.LESSER
 * and COPYING for details.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "websocket.h"
#include "utils.h"
#include "logging.h"
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
/**
 * @brief Initialize compression streams for permessage-deflate
 */
static int websocket_init_compression(websocket_t *ws)
{
    if (!ws || ws->compression_initialized) {
        return WS_SUCCESS;
    }
    
    // Initialize deflate stream (for sending)
    ws->deflate_stream.zalloc = Z_NULL;
    ws->deflate_stream.zfree = Z_NULL;
    ws->deflate_stream.opaque = Z_NULL;
    
    // Use raw deflate (no zlib header)
    int ret = deflateInit2(&ws->deflate_stream, Z_DEFAULT_COMPRESSION,
                          Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        log_error("Failed to initialize deflate stream");
        return WS_ERROR;
    }
    
    // Initialize inflate stream (for receiving)
    ws->inflate_stream.zalloc = Z_NULL;
    ws->inflate_stream.zfree = Z_NULL;
    ws->inflate_stream.opaque = Z_NULL;
    
    ret = inflateInit2(&ws->inflate_stream, -15);
    if (ret != Z_OK) {
        log_error("Failed to initialize inflate stream");
        deflateEnd(&ws->deflate_stream);
        return WS_ERROR;
    }
    
    ws->compression_initialized = 1;
    log_debug("Compression streams initialized");
    return WS_SUCCESS;
}

/**
 * @brief Cleanup compression streams
 */
static void websocket_cleanup_compression(websocket_t *ws)
{
    if (!ws || !ws->compression_initialized) {
        return;
    }
    
    deflateEnd(&ws->deflate_stream);
    inflateEnd(&ws->inflate_stream);
    ws->compression_initialized = 0;
}

/**
 * @brief Compress payload data using deflate
 * @param data Input data
 * @param data_len Length of input
 * @param out Output buffer
 * @param out_size Size of output buffer
 * @return Number of bytes in compressed output, or -1 on error
 */
static ssize_t websocket_compress_payload(websocket_t *ws, const uint8_t *data,
                                          size_t data_len, uint8_t *out, size_t out_size)
{
    if (!ws || !ws->compress_on_send || !ws->compression_initialized) {
        // No compression - just copy
        if (data_len > out_size) return -1;
        memcpy(out, data, data_len);
        return data_len;
    }
    
    ws->deflate_stream.next_in = (Bytef *)data;
    ws->deflate_stream.avail_in = data_len;
    ws->deflate_stream.next_out = out;
    ws->deflate_stream.avail_out = out_size;
    
    int ret = deflate(&ws->deflate_stream, Z_SYNC_FLUSH);
    if (ret != Z_OK && ret != Z_BUF_ERROR) {
        log_error("Deflate failed: %d", ret);
        return -1;
    }
    
    ssize_t compressed_len = out_size - ws->deflate_stream.avail_out;
    
    // Remove trailing 0x00 0x00 0xFF 0xFF per RFC 7692
    if (compressed_len >= 4 &&
        out[compressed_len-4] == 0x00 && out[compressed_len-3] == 0x00 &&
        out[compressed_len-2] == 0xFF && out[compressed_len-1] == 0xFF) {
        compressed_len -= 4;
    }
    
    return compressed_len;
}

/**
 * @brief Decompress payload data using inflate
 * @param data Compressed input data
 * @param data_len Length of compressed input
 * @param out Output buffer
 * @param out_size Size of output buffer
 * @return Number of bytes in decompressed output, or -1 on error
 */
static ssize_t websocket_decompress_payload(websocket_t *ws, uint8_t *data,
                                            size_t data_len, uint8_t *out, size_t out_size)
{
    if (!ws || !ws->compression_initialized) {
        // No decompression - data is already uncompressed
        if (data_len > out_size) return -1;
        if (data != out) {
            memcpy(out, data, data_len);
        }
        return data_len;
    }
    
    // Inflate the payload, then the 0x00 0x00 0xFF 0xFF tail that RFC 7692
    // strips from each message (fed separately to avoid copying the payload)
    static const uint8_t tail[4] = { 0x00, 0x00, 0xFF, 0xFF };
    const uint8_t *inputs[2] = { data, tail };
    size_t input_lens[2] = { data_len, sizeof(tail) };

    ws->inflate_stream.next_out = out;
    ws->inflate_stream.avail_out = out_size;

    for (int i = 0; i < 2; i++) {
        ws->inflate_stream.next_in = (Bytef *)inputs[i];
        ws->inflate_stream.avail_in = input_lens[i];

        int ret = inflate(&ws->inflate_stream, Z_SYNC_FLUSH);
        if (ret != Z_OK && ret != Z_BUF_ERROR) {
            log_error("Inflate failed: %d", ret);
            return -1;
        }

        /* Output buffer full: the message is at least out_size bytes, which
         * callers treat as too big. Report it rather than truncating. */
        if (ws->inflate_stream.avail_out == 0) {
            return (ssize_t)out_size;
        }
    }
    
    return out_size - ws->inflate_stream.avail_out;
}


/**
 * @brief Parse WebSocket key from HTTP headers and perform handshake
 * @param sec_key The Sec-WebSocket-Key header value
 * @param http_headers Array of HTTP header strings
 * @param num_headers Number of headers in array
 */
int websocket_do_handshake(websocket_t *ws, const char *sec_key,
                            const char **http_headers, int num_headers)
{
    if (!ws || !sec_key) {
        return WS_EINVAL;
    }
    
    // Check if client supports permessage-deflate compression
    int client_supports_compression = 0;
    if (http_headers && num_headers > 0) {
        for (int i = 0; i < num_headers; i++) {
            if (strncasecmp(http_headers[i], "Sec-WebSocket-Extensions:", 25) == 0) {
                const char *ext = http_headers[i] + 25;
                if (strstr(ext, "permessage-deflate") != NULL) {
                    client_supports_compression = 1;
                    log_debug("Client supports permessage-deflate compression");
                    break;
                }
            }
        }
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
    
    // Only accept permessage-deflate if the client offered it (RFC 6455 9.1)
    if (client_supports_compression &&
        websocket_init_compression(ws) == WS_SUCCESS) {
        ws->compression_negotiated = 1;
    }

    // Send HTTP 101 Switching Protocols response
    char response[512];
    int len = snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "%s"
        "\r\n",
        accept_key,
        ws->compression_negotiated ?
            "Sec-WebSocket-Extensions: permessage-deflate; server_no_context_takeover; client_no_context_takeover\r\n" : "");
    
    if (io_send_all(ws->sock_fd, ws->ssl, (uint8_t *)response, len) != len) {
        log_error("Failed to send WebSocket handshake response");
        return WS_ESOCKET;
    }
    
    log_info("WebSocket handshake completed (compression: %s)",
             ws->compression_negotiated ? "permessage-deflate" : "none");
    ws->state = WS_STATE_OPEN;
    
    // DECISION: Don't compress outgoing frames for VNC protocol compatibility
    // Rationale: VNC uses binary protocol that doesn't compress well and adds overhead.
    //           We will still decompress any incoming compressed frames from the client.
    // Future: If compression is needed, set compress_on_send=1 and enable the
    //         websocket_send_frame_FIXME_compressed logic.
    ws->compress_on_send = 0;  // Disabled for VNC compatibility
    
    return WS_SUCCESS;
#pragma GCC diagnostic pop
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
        log_debug("Attempt to send on non-open WebSocket");
        return -1;
    }
    
    // Room for the largest header (14 bytes) plus a full-size payload
    uint8_t frame[MAX_FRAME_SIZE + 14];
    size_t frame_len = 0;

    if (data_len > MAX_FRAME_SIZE) {
        log_error("Frame too large: %zu bytes", data_len);
        return -1;
    }
    
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
        memcpy(frame + frame_len, data, data_len);
        frame_len += data_len;
    }
    
    // Send the frame
    if (io_send_all(ws->sock_fd, ws->ssl, frame, frame_len) != (ssize_t)frame_len) {
        log_error("Failed to send WebSocket frame");
        ws->state = WS_STATE_CLOSED;
        return -1;
    }

    // Track wire bytes (compressed frame size)
    ws->bytes_sent_wire += frame_len;

    return data_len;
}

/**
 * @brief Encode and send a WebSocket frame
 */
ssize_t websocket_send_frame_FIXME_compressed(websocket_t *ws, const uint8_t *data,
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
    uint8_t compressed[MAX_FRAME_SIZE];
    size_t frame_len = 0;
    const uint8_t *payload = data;
    size_t payload_len = data_len;
    int rsv1 = 0;
    
    // Try to compress payload if compression is enabled and opcode is binary/text
    if (ws->compress_on_send && ws->compression_initialized &&
        (opcode == WS_OPCODE_BINARY || opcode == WS_OPCODE_TEXT) &&
        data && data_len > 0) {
        ssize_t comp_len = websocket_compress_payload(ws, data, data_len, 
                                                     compressed, sizeof(compressed));
        if (comp_len > 0 && comp_len < (ssize_t)data_len) {
            payload = compressed;
            payload_len = comp_len;
            rsv1 = 1;  // Set RSV1 bit for compression
            log_debug("Compressed outgoing payload: %zu -> %zd bytes", data_len, comp_len);
        }
    }
    
    // Byte 0: FIN + RSV1 + opcode
    frame[frame_len++] = (fin ? 0x80 : 0x00) | (rsv1 ? 0x40 : 0x00) | (opcode & 0x0F);
    
    // Byte 1: MASK + payload length
    // Server-to-client frames are NOT masked (MASK=0)
    if (payload_len < 126) {
        frame[frame_len++] = (uint8_t)payload_len;
    } else if (payload_len < 65536) {
        frame[frame_len++] = 126;
        frame[frame_len++] = (payload_len >> 8) & 0xFF;
        frame[frame_len++] = payload_len & 0xFF;
    } else {
        frame[frame_len++] = 127;
        for (int i = 7; i >= 0; i--) {
            frame[frame_len++] = (payload_len >> (i * 8)) & 0xFF;
        }
    }
    
    // Copy payload data (compressed or original)
    if (payload && payload_len > 0) {
        if (frame_len + payload_len > MAX_FRAME_SIZE) {
            log_error("Frame too large: %zu bytes", frame_len + payload_len);
            return -1;
        }
        memcpy(frame + frame_len, payload, payload_len);
        frame_len += payload_len;
    }
    
    // Send the frame
    if (io_send_all(ws->sock_fd, ws->ssl, frame, frame_len) != (ssize_t)frame_len) {
        log_error("Failed to send WebSocket frame");
        return -1;
    }
    
    // Track wire bytes (compressed frame size)
    ws->bytes_sent_wire += frame_len;
    
    return data_len;
}

/**
 * @brief Fail the connection with a close frame (RFC 6455 section 7.1.7)
 */
static ssize_t websocket_fail(websocket_t *ws, uint16_t code, const char *why)
{
    log_warn("Closing WebSocket (%u): %s", code, why);
    websocket_close(ws, code, NULL);
    ws->state = WS_STATE_CLOSED;
    return -1;
}

/**
 * @brief Discard any partially reassembled message
 */
static void websocket_reset_fragments(websocket_t *ws)
{
    free(ws->partial_msg);
    ws->partial_msg = NULL;
    ws->partial_msg_len = 0;
    ws->partial_msg_capacity = 0;
    ws->fragmented_opcode = 0;
    ws->msg_compressed = 0;
}

/**
 * @brief Receive and decode a WebSocket frame
 *
 * Reads one frame. Control frames are handled internally, and fragmented
 * messages are reassembled (and decompressed) before being returned.
 *
 * @return Length of a complete data message (possibly 0), 0 when a control
 *         frame or non-final fragment was consumed, or -1 on error. When the
 *         session ends, ws->state is no longer WS_STATE_OPEN.
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
    
    uint8_t header[8];
    size_t header_len = 2;

    // Read the fixed 2-byte header; failure here means the peer went away
    if (io_recv_exact(ws->sock_fd, ws->ssl, header, 2) != WS_SUCCESS) {
        log_debug("WebSocket peer closed the connection");
        ws->state = WS_STATE_CLOSED;
        return 0;
    }
    
    // Parse header byte 0
    int fin = (header[0] & 0x80) != 0;
    int rsv1 = (header[0] & 0x40) != 0;  // Compression flag
    uint8_t opcode = header[0] & 0x0F;
    int is_control = (opcode & 0x08) != 0;
    
    // Parse header byte 1
    int masked = (header[1] & 0x80) != 0;
    uint64_t payload_len = header[1] & 0x7F;

    if (header[0] & 0x30) {
        return websocket_fail(ws, 1002, "reserved bits RSV2/RSV3 set");
    }
    if (rsv1 && (!ws->compression_negotiated || is_control ||
                 opcode == WS_OPCODE_CONTINUATION)) {
        return websocket_fail(ws, 1002, "unexpected RSV1 bit");
    }
    if (!masked) {
        return websocket_fail(ws, 1002, "client frame is not masked");
    }
    if (is_control && (!fin || payload_len > 125)) {
        return websocket_fail(ws, 1002, "invalid control frame");
    }
    
    // Read extended payload length if needed
    if (payload_len == 126) {
        if (io_recv_exact(ws->sock_fd, ws->ssl, header, 2) != WS_SUCCESS) {
            ws->state = WS_STATE_CLOSED;
            return -1;
        }
        payload_len = ((uint64_t)header[0] << 8) | header[1];
        header_len += 2;
    } else if (payload_len == 127) {
        if (io_recv_exact(ws->sock_fd, ws->ssl, header, 8) != WS_SUCCESS) {
            ws->state = WS_STATE_CLOSED;
            return -1;
        }
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | header[i];
        }
        header_len += 8;
    }
    
    // Read masking key
    uint8_t mask[4];
    if (io_recv_exact(ws->sock_fd, ws->ssl, mask, 4) != WS_SUCCESS) {
        ws->state = WS_STATE_CLOSED;
        return -1;
    }
    header_len += 4;
    
    // Check payload size
    if (payload_len > data_len) {
        log_error("Payload too large: %lu bytes (buffer: %zu)", 
                 (unsigned long)payload_len, data_len);
        return websocket_fail(ws, 1009, "message too big");
    }
    
    // Read and unmask payload data
    if (payload_len > 0) {
        if (io_recv_exact(ws->sock_fd, ws->ssl, data_out, payload_len) != WS_SUCCESS) {
            log_error("Failed to read payload data");
            ws->state = WS_STATE_CLOSED;
            return -1;
        }
        for (size_t i = 0; i < payload_len; i++) {
            data_out[i] ^= mask[i % 4];
        }
    }

    // Track wire bytes (frame as received)
    ws->bytes_received_wire += header_len + payload_len;
    
    // Handle control frames
    if (opcode == WS_OPCODE_CLOSE) {
        uint16_t code = 1000;
        if (payload_len >= 2) {
            code = (uint16_t)((data_out[0] << 8) | data_out[1]);
        }
        ws->close_received = 1;
        ws->close_code = code;
        log_debug("Received close frame (code %u)", code);
        websocket_close(ws, code, NULL);  // Echo the close frame
        ws->state = WS_STATE_CLOSED;
        return 0;
    } else if (opcode == WS_OPCODE_PING) {
        websocket_send_frame(ws, data_out, payload_len, WS_OPCODE_PONG, 1);
        return 0;
    } else if (opcode == WS_OPCODE_PONG) {
        return 0;
    } else if (is_control) {
        return websocket_fail(ws, 1002, "unknown control opcode");
    }

    // Data frames: reassemble fragmented messages
    uint8_t message_opcode = opcode;
    int message_compressed = rsv1;
    size_t message_len = payload_len;

    if (opcode == WS_OPCODE_CONTINUATION) {
        if (!ws->partial_msg) {
            return websocket_fail(ws, 1002, "continuation without initial frame");
        }
        
        size_t new_len = ws->partial_msg_len + payload_len;
        if (new_len > data_len) {
            websocket_reset_fragments(ws);
            return websocket_fail(ws, 1009, "reassembled message too big");
        }
        if (new_len > ws->partial_msg_capacity) {
            size_t new_cap = new_len * 2;
            uint8_t *new_buf = realloc(ws->partial_msg, new_cap);
            if (!new_buf) {
                log_error("Failed to realloc partial message buffer");
                websocket_reset_fragments(ws);
                return websocket_fail(ws, 1011, "out of memory");
            }
            ws->partial_msg = new_buf;
            ws->partial_msg_capacity = new_cap;
        }
        
        memcpy(ws->partial_msg + ws->partial_msg_len, data_out, payload_len);
        ws->partial_msg_len = new_len;
        
        if (!fin) {
            return 0;  // More fragments coming
        }

        // Final fragment - the complete message goes to data_out
        memcpy(data_out, ws->partial_msg, ws->partial_msg_len);
        message_len = ws->partial_msg_len;
        message_opcode = ws->fragmented_opcode;
        message_compressed = ws->msg_compressed;
        websocket_reset_fragments(ws);
    } else if (opcode == WS_OPCODE_TEXT || opcode == WS_OPCODE_BINARY) {
        if (ws->partial_msg) {
            websocket_reset_fragments(ws);
            return websocket_fail(ws, 1002, "new message inside a fragmented message");
        }
        if (!fin) {
            // First frame of a fragmented message
            ws->fragmented_opcode = opcode;
            ws->msg_compressed = (uint8_t)rsv1;
            ws->partial_msg_capacity = payload_len > 0 ? payload_len * 2 : 1024;
            ws->partial_msg = malloc(ws->partial_msg_capacity);
            if (!ws->partial_msg) {
                log_error("Failed to allocate partial message buffer");
                return websocket_fail(ws, 1011, "out of memory");
            }
            memcpy(ws->partial_msg, data_out, payload_len);
            ws->partial_msg_len = payload_len;
            return 0;  // Waiting for more fragments
        }
    } else {
        return websocket_fail(ws, 1002, "unknown data opcode");
    }

    // Decompress the complete message (permessage-deflate, RFC 7692)
    if (message_compressed && message_len > 0) {
        /* One spare byte lets us detect output that would not fit */
        if (ws->inflate_buf_size < data_len + 1) {
            free(ws->inflate_buf);
            ws->inflate_buf = malloc(data_len + 1);
            ws->inflate_buf_size = ws->inflate_buf ? data_len + 1 : 0;
            if (!ws->inflate_buf) {
                return websocket_fail(ws, 1011, "out of memory");
            }
        }
        ssize_t decomp_len = websocket_decompress_payload(ws, data_out, message_len,
                                                         ws->inflate_buf, data_len + 1);
        if (decomp_len < 0) {
            return websocket_fail(ws, 1007, "invalid compressed data");
        }
        if ((size_t)decomp_len > data_len) {
            return websocket_fail(ws, 1009, "decompressed message too big");
        }
        log_debug("Decompressed WebSocket payload: %zu -> %zd bytes",
                 message_len, decomp_len);
        memcpy(data_out, ws->inflate_buf, decomp_len);
        message_len = (size_t)decomp_len;
    }
    
    if (opcode_out) {
        *opcode_out = message_opcode;
    }
    
    return (ssize_t)message_len;
}
