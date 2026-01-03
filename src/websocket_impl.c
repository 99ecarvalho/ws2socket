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
    if (!ws || !ws->compression_enabled || !ws->compression_initialized) {
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
    if (!ws || !ws->compression_enabled || !ws->compression_initialized) {
        // No decompression - data is already uncompressed
        if (data_len > out_size) return -1;
        if (data != out) {
            memcpy(out, data, data_len);
        }
        return data_len;
    }
    
    // Add back the 0x00 0x00 0xFF 0xFF tail per RFC 7692
    uint8_t temp_buf[data_len + 4];
    memcpy(temp_buf, data, data_len);
    temp_buf[data_len] = 0x00;
    temp_buf[data_len + 1] = 0x00;
    temp_buf[data_len + 2] = 0xFF;
    temp_buf[data_len + 3] = 0xFF;
    
    ws->inflate_stream.next_in = temp_buf;
    ws->inflate_stream.avail_in = data_len + 4;
    ws->inflate_stream.next_out = out;
    ws->inflate_stream.avail_out = out_size;
    
    int ret = inflate(&ws->inflate_stream, Z_SYNC_FLUSH);
    if (ret != Z_OK && ret != Z_BUF_ERROR) {
        log_error("Inflate failed: %d", ret);
        return -1;
    }
    
    return out_size - ws->inflate_stream.avail_out;
}


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
        "Sec-WebSocket-Extensions: permessage-deflate; server_no_context_takeover; client_no_context_takeover\r\n"
        "\r\n",
        accept_key);
    
    ssize_t sent = socket_send(ws->sock_fd, (uint8_t *)response, len, 0);
    if (sent != len) {
        log_error("Failed to send WebSocket handshake response");
        return WS_ESOCKET;
    }
    
    log_info("WebSocket handshake completed");
    ws->state = WS_STATE_OPEN;
    
    // Enable compression
    ws->compression_enabled = 1;
    websocket_init_compression(ws);
    
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
    int rsv1 = (header[0] & 0x40) != 0;  // Compression flag
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
    
    // Decompress payload if RSV1 bit is set (per-message deflate)
    if (rsv1 && payload_len > 0) {
        // Need a temporary buffer for decompression
        uint8_t decomp_buf[65536];  // 64KB buffer for decompressed data
        ssize_t decomp_len = websocket_decompress_payload(ws, data_out, payload_len, 
                                                         decomp_buf, sizeof(decomp_buf));
        if (decomp_len < 0) {
            log_error("Failed to decompress WebSocket payload");
            return -1;
        }
        
        // Copy decompressed data back to output buffer
        if (decomp_len > (ssize_t)data_len) {
            log_error("Decompressed payload too large: %zd bytes (buffer: %zu)", 
                     decomp_len, data_len);
            return -1;
        }
        
        memcpy(data_out, decomp_buf, decomp_len);
        payload_len = decomp_len;
        log_debug("Decompressed WebSocket payload: %zd -> %zd bytes", 
                 (ssize_t)payload_len, decomp_len);
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
    
    // Handle fragmented messages (continuation frames)
    if (opcode == WS_OPCODE_CONTINUATION) {
        // This is a continuation frame
        if (!ws->partial_msg) {
            log_error("Received continuation frame without initial frame");
            return -1;
        }
        
        // Append to partial message
        size_t new_len = ws->partial_msg_len + payload_len;
        if (new_len > ws->partial_msg_capacity) {
            size_t new_cap = new_len * 2;
            uint8_t *new_buf = realloc(ws->partial_msg, new_cap);
            if (!new_buf) {
                log_error("Failed to realloc partial message buffer");
                return -1;
            }
            ws->partial_msg = new_buf;
            ws->partial_msg_capacity = new_cap;
        }
        
        memcpy(ws->partial_msg + ws->partial_msg_len, data_out, payload_len);
        ws->partial_msg_len = new_len;
        
        if (fin) {
            // Final fragment - return complete message
            if (ws->partial_msg_len > data_len) {
                log_error("Reassembled message too large");
                free(ws->partial_msg);
                ws->partial_msg = NULL;
                ws->partial_msg_len = 0;
                ws->partial_msg_capacity = 0;
                return -1;
            }
            memcpy(data_out, ws->partial_msg, ws->partial_msg_len);
            ssize_t total_len = ws->partial_msg_len;
            if (opcode_out) {
                *opcode_out = ws->fragmented_opcode;
            }
            free(ws->partial_msg);
            ws->partial_msg = NULL;
            ws->partial_msg_len = 0;
            ws->partial_msg_capacity = 0;
            ws->fragmented_opcode = 0;
            return total_len;
        } else {
            // More fragments coming
            return 0;
        }
    } else if (!fin) {
        // First frame of a fragmented message
        if (ws->partial_msg) {
            log_warn("Starting new fragmented message while one is in progress");
            free(ws->partial_msg);
        }
        
        ws->fragmented_opcode = opcode;
        ws->partial_msg_capacity = payload_len * 2;
        ws->partial_msg = malloc(ws->partial_msg_capacity);
        if (!ws->partial_msg) {
            log_error("Failed to allocate partial message buffer");
            return -1;
        }
        
        memcpy(ws->partial_msg, data_out, payload_len);
        ws->partial_msg_len = payload_len;
        return 0;  // Waiting for more fragments
    }
    // Single complete frame
    
    return payload_len;
}
