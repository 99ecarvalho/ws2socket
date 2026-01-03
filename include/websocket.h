/**
 * @file websocket.h
 * @brief WebSocket Protocol Implementation (RFC 6455)
 * @author WebSocket to TCP Proxy Project
 * @version 0.1.0
 * 
 * Implements the WebSocket protocol (RFC 6455) with support for:
 * - WebSocket handshake (client and server)
 * - Frame encoding/decoding
 * - Masking/unmasking
 * - Close frames, ping, pong
 * - Continuation frames
 * 
 * License: LGPL v3
 */

#ifndef WS2SOCKET_WEBSOCKET_H
#define WS2SOCKET_WEBSOCKET_H

#include "common.h"
#include <openssl/ssl.h>
#include <zlib.h>

/**
 * @struct websocket
 * @brief WebSocket connection object
 * 
 * Represents a WebSocket connection with all necessary state and buffers.
 */
typedef struct websocket {
    /** Connection state (see @ref WebSocketStates) */
    int state;
    /** Underlying TCP socket file descriptor */
    int sock_fd;
    /** SSL connection (if using WSS) */
    SSL *ssl;
    /** Receive buffer */
    ws_buffer_t recv_buf;
    /** Send buffer */
    ws_buffer_t send_buf;
    /** Partial message buffer for fragmented frames */
    uint8_t *partial_msg;
    /** Length of partial message */
    size_t partial_msg_len;
    /** Allocated size of partial message buffer */
    size_t partial_msg_capacity;
    /** Opcode of the first frame in a fragmented message */
    uint8_t fragmented_opcode;
    /** Compression enabled flag */
    uint8_t compression_enabled;
    /** Compression initialized */
    uint8_t compression_initialized;
    /** Deflate stream for compression */
    z_stream deflate_stream;
    /** Inflate stream for decompression */
    z_stream inflate_stream;
    /** Close code (when closed) */
    uint16_t close_code;
    /** Close reason (when closed) */
    char *close_reason;
    /** Flag: close frame sent */
    uint8_t close_sent;
    /** Flag: close frame received */
    uint8_t close_received;
    /** HTTP headers from client */
    char *headers;
    /** Selected WebSocket sub-protocol */
    char *protocol;
    /** Last frame was masked (for validation) */
    uint8_t last_was_masked;
    /** Total bytes received on wire (compressed) */
    uint64_t bytes_received_wire;
    /** Total bytes sent on wire (compressed) */
    uint64_t bytes_sent_wire;
} websocket_t;

/**
 * @brief Create a new WebSocket connection object
 * 
 * Allocates and initializes a new WebSocket structure.
 * 
 * @return Pointer to new websocket_t structure, NULL on failure
 * 
 * @note Remember to call websocket_destroy() to free resources
 */
websocket_t *websocket_create(void);

/**
 * @brief Destroy a WebSocket connection object
 * 
 * Frees all resources associated with a WebSocket connection.
 * The underlying socket should be closed before calling this.
 * 
 * @param ws Pointer to websocket_t to destroy
 */
void websocket_destroy(websocket_t *ws);

/**
 * @brief Initialize WebSocket with a socket and optional SSL
 * 
 * Sets up a WebSocket connection with the given TCP socket and
 * optional SSL/TLS context.
 * 
 * @param ws WebSocket connection object
 * @param sock_fd Socket file descriptor
 * @param ssl SSL connection handle (NULL for plain WebSocket)
 * @return WS_SUCCESS on success, error code otherwise
 */
int websocket_init(websocket_t *ws, int sock_fd, SSL *ssl);

/**
 * @brief Accept WebSocket connection from client
 * 
 * Performs the WebSocket handshake as a server, parsing the HTTP
 * upgrade request and sending the appropriate response.
 * 
 * @param ws WebSocket connection object
 * @param http_headers HTTP headers from the client request
 * @param num_headers Number of header lines
 * @return WS_SUCCESS on success, error code otherwise
 * 
 * @note After successful handshake, state becomes WS_STATE_OPEN
 */
int websocket_accept(websocket_t *ws, const char **http_headers, 
                     int num_headers);

/**
 * @brief Connect WebSocket to server (client-side)
 * 
 * Performs the WebSocket handshake as a client, sending the upgrade
 * request and parsing the server response.
 * 
 * @param ws WebSocket connection object
 * @param host Target hostname/IP
 * @param port Target port
 * @param path Request path (e.g., "/socket")
 * @param protocols Optional array of protocol names to request
 * @param num_protocols Number of protocols in array
 * @return WS_SUCCESS on success, error code otherwise
 */
int websocket_connect(websocket_t *ws, const char *host, uint16_t port,
                     const char *path, const char **protocols,
                     int num_protocols);

/**
 * @brief Send data as WebSocket frame
 * 
 * Encodes data as a WebSocket frame and sends it to the peer.
 * Handles masking for client-to-server frames.
 * 
 * @param ws WebSocket connection object
 * @param data Data to send
 * @param data_len Length of data
 * @param opcode Frame opcode (WS_OPCODE_TEXT or WS_OPCODE_BINARY)
 * @return Number of bytes sent, negative on error
 * 
 * @note For large payloads, may return less than data_len if socket
 *       is not ready. Caller should retry with remaining data.
 */
ssize_t websocket_send(websocket_t *ws, const uint8_t *data,
                      size_t data_len, uint8_t opcode);

/**
 * @brief Receive data from WebSocket frame
 * 
 * Receives and decodes WebSocket frames, returning application data.
 * Automatically handles control frames (ping, pong, close).
 * 
 * @param ws WebSocket connection object
 * @param data Buffer to store received data
 * @param data_len Maximum data to read
 * @return Number of bytes read, 0 if connection closed, negative on error
 * 
 * @note Caller should check for WS_STATE_CLOSED to detect close frames
 */
ssize_t websocket_recv(websocket_t *ws, uint8_t *data, size_t data_len);

/**
 * @brief Send WebSocket ping frame
 * 
 * Sends a control frame ping with optional data.
 * 
 * @param ws WebSocket connection object
 * @param data Optional ping payload (NULL for empty)
 * @param data_len Length of ping payload (0 if NULL)
 * @return WS_SUCCESS on success, error code otherwise
 */
int websocket_ping(websocket_t *ws, const uint8_t *data, size_t data_len);

/**
 * @brief Send WebSocket pong frame
 * 
 * Sends a control frame pong with optional data.
 * 
 * @param ws WebSocket connection object
 * @param data Optional pong payload (NULL for empty)
 * @param data_len Length of pong payload (0 if NULL)
 * @return WS_SUCCESS on success, error code otherwise
 */
int websocket_pong(websocket_t *ws, const uint8_t *data, size_t data_len);

/**
 * @brief Close WebSocket connection gracefully
 * 
 * Sends a close frame and waits for peer to acknowledge.
 * Sets connection state to WS_STATE_CLOSED.
 * 
 * @param ws WebSocket connection object
 * @param code Close code (1000-4999, 1000=normal)
 * @param reason Optional close reason text (NULL for none)
 * @return WS_SUCCESS on success, error code otherwise
 */
int websocket_close(websocket_t *ws, uint16_t code, const char *reason);

/**
 * @brief Check if connection has pending data
 * 
 * Returns true if there is buffered data ready to be read.
 * 
 * @param ws WebSocket connection object
 * @return 1 if data pending, 0 otherwise
 */
int websocket_pending(const websocket_t *ws);

/**
 * @brief Get WebSocket state
 * 
 * @param ws WebSocket connection object
 * @return Current state (see @ref WebSocketStates)
 */
int websocket_get_state(const websocket_t *ws);

/**
 * @brief Get peer name (remote address)
 * 
 * @param ws WebSocket connection object
 * @param addr_out Pointer to store address
 * @param addr_len Pointer to store address length
 * @return WS_SUCCESS on success, error code otherwise
 */
int websocket_getpeername(const websocket_t *ws, struct sockaddr *addr_out,
                         socklen_t *addr_len);

/**
 * @brief Get socket name (local address)
 * 
 * @param ws WebSocket connection object
 * @param addr_out Pointer to store address
 * @param addr_len Pointer to store address length
 * @return WS_SUCCESS on success, error code otherwise
 */
int websocket_getsockname(const websocket_t *ws, struct sockaddr *addr_out,
                         socklen_t *addr_len);

/**
 * @brief Set socket timeout
 * 
 * Sets the socket send/receive timeout.
 * 
 * @param ws WebSocket connection object
 * @param timeout_sec Timeout in seconds
 * @return WS_SUCCESS on success, error code otherwise
 */
int websocket_set_timeout(websocket_t *ws, int timeout_sec);

/**
 * @brief Set non-blocking mode
 * 
 * Enables or disables non-blocking mode for the socket.
 * 
 * @param ws WebSocket connection object
 * @param blocking 1 for blocking, 0 for non-blocking
 * @return WS_SUCCESS on success, error code otherwise
 */
int websocket_set_blocking(websocket_t *ws, int blocking);

#endif /* WS2SOCKET_WEBSOCKET_H */
