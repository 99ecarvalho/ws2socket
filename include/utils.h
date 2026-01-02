/**
 * @file utils.h
 * @brief Utility functions
 * @author WebSocket to TCP Proxy Project
 * @version 0.1.0
 * 
 * Provides various utility functions including string handling,
 * base64 encoding, SHA1 hashing, and socket utilities.
 * 
 * License: LGPL v3
 */

#ifndef WS2SOCKET_UTILS_H
#define WS2SOCKET_UTILS_H

#include "common.h"
#include <stddef.h>

/**
 * @defgroup StringFunctions String Functions
 * @{
 */

/**
 * @brief Case-insensitive string comparison
 * 
 * @param s1 First string
 * @param s2 Second string
 * @return 0 if equal, <0 or >0 if different
 */
int strcasecmp_safe(const char *s1, const char *s2);

/**
 * @brief Safe string copy
 * 
 * @param dest Destination buffer
 * @param src Source string
 * @param dest_size Size of destination buffer
 * @return Number of bytes copied (excluding null terminator)
 */
size_t strlcpy(char *dest, const char *src, size_t dest_size);

/**
 * @brief Parse hostname and port from string
 * 
 * @param hostport String in format "host:port" or "host"
 * @param host_out Buffer to store hostname
 * @param host_out_size Size of host buffer
 * @param port_out Pointer to store parsed port (0 if not specified)
 * @return WS_SUCCESS on success, error code otherwise
 */
int parse_hostport(const char *hostport, char *host_out, size_t host_out_size,
                   uint16_t *port_out);

/**
 * @brief Trim whitespace from string
 * 
 * @param str String to trim
 * @return Pointer to trimmed string (may be same as input)
 * 
 * @note Modifies string in-place
 */
char *strtrim(char *str);

/** @} */

/**
 * @defgroup Base64Functions Base64 Encoding/Decoding
 * @{
 */

/**
 * @brief Base64 encode data
 * 
 * @param data Data to encode
 * @param data_len Length of data
 * @param encoded_out Buffer to store encoded data
 * @param encoded_out_size Size of output buffer
 * @return Length of encoded data, negative on error
 */
ssize_t base64_encode(const uint8_t *data, size_t data_len,
                     char *encoded_out, size_t encoded_out_size);

/**
 * @brief Base64 decode data
 * 
 * @param encoded Encoded data string
 * @param data_out Buffer to store decoded data
 * @param data_out_size Size of output buffer
 * @return Length of decoded data, negative on error
 */
ssize_t base64_decode(const char *encoded, uint8_t *data_out,
                     size_t data_out_size);

/** @} */

/**
 * @defgroup CryptoFunctions Cryptographic Functions
 * @{
 */

/**
 * @brief Calculate SHA1 hash
 * 
 * @param data Data to hash
 * @param data_len Length of data
 * @param digest_out Buffer to store digest (must be 20 bytes)
 * @return WS_SUCCESS on success, error code otherwise
 */
int sha1_digest(const uint8_t *data, size_t data_len,
               uint8_t *digest_out);

/**
 * @brief Generate random bytes
 * 
 * @param buffer Buffer to fill
 * @param size Number of bytes to generate
 * @return WS_SUCCESS on success, error code otherwise
 */
int random_bytes(uint8_t *buffer, size_t size);

/** @} */

/**
 * @defgroup SocketFunctions Socket Functions
 * @{
 */

/**
 * @brief Create TCP socket
 * 
 * @param blocking 1 for blocking, 0 for non-blocking
 * @return Socket file descriptor, negative on error
 */
int socket_create_tcp(int blocking);

/**
 * @brief Connect socket to host:port
 * 
 * @param sock_fd Socket file descriptor
 * @param host Hostname or IP address
 * @param port Port number
 * @param timeout_sec Connection timeout (0 for no timeout)
 * @return WS_SUCCESS on success, error code otherwise
 */
int socket_connect(int sock_fd, const char *host, uint16_t port,
                  int timeout_sec);

/**
 * @brief Bind socket to address
 * 
 * @param sock_fd Socket file descriptor
 * @param host Hostname or IP address (use "0.0.0.0" or "::" for any)
 * @param port Port number
 * @return WS_SUCCESS on success, error code otherwise
 */
int socket_bind(int sock_fd, const char *host, uint16_t port);

/**
 * @brief Set socket to listening state
 * 
 * @param sock_fd Socket file descriptor
 * @param backlog Listen backlog size
 * @return WS_SUCCESS on success, error code otherwise
 */
int socket_listen(int sock_fd, int backlog);

/**
 * @brief Accept incoming connection
 * 
 * @param listen_fd Listening socket file descriptor
 * @param addr_out Pointer to store client address
 * @param addr_len_out Pointer to store address length
 * @return New socket file descriptor, negative on error
 */
int socket_accept(int listen_fd, struct sockaddr_storage *addr_out,
                 socklen_t *addr_len_out);

/**
 * @brief Send data on socket
 * 
 * @param sock_fd Socket file descriptor
 * @param data Data to send
 * @param data_len Length of data
 * @param flags Send flags (MSG_NOSIGNAL, etc)
 * @return Number of bytes sent, negative on error
 */
ssize_t socket_send(int sock_fd, const uint8_t *data, size_t data_len,
                   int flags);

/**
 * @brief Receive data from socket
 * 
 * @param sock_fd Socket file descriptor
 * @param buffer Buffer to store data
 * @param buffer_size Size of buffer
 * @param flags Receive flags
 * @return Number of bytes received, 0 on close, negative on error
 */
ssize_t socket_recv(int sock_fd, uint8_t *buffer, size_t buffer_size,
                   int flags);

/**
 * @brief Shutdown socket
 * 
 * @param sock_fd Socket file descriptor
 * @param how How to shutdown (SHUT_RD, SHUT_WR, SHUT_RDWR)
 * @return WS_SUCCESS on success, error code otherwise
 */
int socket_shutdown(int sock_fd, int how);

/**
 * @brief Close socket
 * 
 * @param sock_fd Socket file descriptor
 * @return WS_SUCCESS on success, error code otherwise
 */
int socket_close(int sock_fd);

/**
 * @brief Set socket to non-blocking
 * 
 * @param sock_fd Socket file descriptor
 * @return WS_SUCCESS on success, error code otherwise
 */
int socket_set_nonblocking(int sock_fd);

/**
 * @brief Set socket timeout
 * 
 * @param sock_fd Socket file descriptor
 * @param timeout_sec Timeout in seconds
 * @return WS_SUCCESS on success, error code otherwise
 */
int socket_set_timeout(int sock_fd, int timeout_sec);

/**
 * @brief Set TCP_NODELAY option
 * 
 * @param sock_fd Socket file descriptor
 * @param enabled 1 to enable, 0 to disable
 * @return WS_SUCCESS on success, error code otherwise
 */
int socket_set_nodelay(int sock_fd, int enabled);

/**
 * @brief Set SO_KEEPALIVE option
 * 
 * @param sock_fd Socket file descriptor
 * @param enabled 1 to enable, 0 to disable
 * @return WS_SUCCESS on success, error code otherwise
 */
int socket_set_keepalive(int sock_fd, int enabled);

/**
 * @brief Format address to string
 * 
 * @param addr Socket address
 * @param addr_len Address length
 * @param str_out Buffer to store formatted address
 * @param str_size Size of output buffer
 * @return WS_SUCCESS on success, error code otherwise
 */
int socket_addr_to_string(const struct sockaddr_storage *addr,
                         socklen_t addr_len, char *str_out,
                         size_t str_size);

/** @} */

/**
 * @defgroup BufferFunctions Buffer Management
 * @{
 */

/**
 * @brief Initialize circular buffer
 * 
 * @param buf Pointer to buffer structure
 * @param capacity Buffer capacity
 * @return WS_SUCCESS on success, error code otherwise
 */
int buffer_init(ws_buffer_t *buf, size_t capacity);

/**
 * @brief Destroy circular buffer
 * 
 * @param buf Pointer to buffer structure
 */
void buffer_destroy(ws_buffer_t *buf);

/**
 * @brief Write data to buffer
 * 
 * @param buf Pointer to buffer structure
 * @param data Data to write
 * @param len Length of data
 * @return Number of bytes written, -1 if buffer full
 */
ssize_t buffer_write(ws_buffer_t *buf, const uint8_t *data, size_t len);

/**
 * @brief Read data from buffer
 * 
 * @param buf Pointer to buffer structure
 * @param data Buffer to read into
 * @param len Maximum bytes to read
 * @return Number of bytes read
 */
ssize_t buffer_read(ws_buffer_t *buf, uint8_t *data, size_t len);

/**
 * @brief Get number of bytes available in buffer
 * 
 * @param buf Pointer to buffer structure
 * @return Number of bytes available
 */
size_t buffer_available(const ws_buffer_t *buf);

/**
 * @brief Check if buffer is empty
 * 
 * @param buf Pointer to buffer structure
 * @return 1 if empty, 0 otherwise
 */
int buffer_is_empty(const ws_buffer_t *buf);

/**
 * @brief Clear buffer
 * 
 * @param buf Pointer to buffer structure
 */
void buffer_clear(ws_buffer_t *buf);

/** @} */

#endif /* WS2SOCKET_UTILS_H */
