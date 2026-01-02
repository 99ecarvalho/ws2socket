/**
 * @file utils.c
 * @brief Utility functions implementation
 * @author WebSocket to TCP Proxy Project
 * @version 0.1.0
 * 
 * License: LGPL v3
 */

#include "utils.h"
#include "logging.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>

/* =============================================================================
 * String Functions
 * =============================================================================
 */

/**
 * @brief Case-insensitive string comparison
 */
int strcasecmp_safe(const char *s1, const char *s2)
{
    if (!s1 || !s2) {
        return (s1 == s2) ? 0 : 1;
    }

    while (*s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) {
            return c1 - c2;
        }
        s1++;
        s2++;
    }

    return (unsigned char)tolower(*s1) - (unsigned char)tolower(*s2);
}

/**
 * @brief Safe string copy
 */
size_t strlcpy(char *dest, const char *src, size_t dest_size)
{
    size_t len = 0;

    if (!dest || !src || dest_size == 0) {
        return 0;
    }

    while (src[len] && len < dest_size - 1) {
        dest[len] = src[len];
        len++;
    }

    dest[len] = '\0';
    return len;
}

/**
 * @brief Parse hostname and port from string
 */
int parse_hostport(const char *hostport, char *host_out, size_t host_out_size,
                   uint16_t *port_out)
{
    const char *colon;
    size_t host_len;
    long port_num;
    char *endptr;

    if (!hostport || !host_out || host_out_size == 0 || !port_out) {
        return WS_EINVAL;
    }

    /* Find the colon separator */
    colon = strrchr(hostport, ':');

    if (colon) {
        /* Port specified */
        host_len = colon - hostport;
        if (host_len >= host_out_size) {
            return WS_EINVAL;
        }

        strncpy(host_out, hostport, host_len);
        host_out[host_len] = '\0';

        port_num = strtol(colon + 1, &endptr, 10);
        if (*endptr != '\0' || port_num < 1 || port_num > 65535) {
            return WS_EINVAL;
        }

        *port_out = (uint16_t)port_num;
    } else {
        /* No port specified */
        strlcpy(host_out, hostport, host_out_size);
        *port_out = 0;
    }

    return WS_SUCCESS;
}

/**
 * @brief Trim whitespace from string
 */
char *strtrim(char *str)
{
    char *start, *end;

    if (!str) {
        return str;
    }

    /* Skip leading whitespace */
    start = str;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }

    /* Skip trailing whitespace */
    end = start + strlen(start) - 1;
    while (end >= start && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    /* Shift to beginning if needed */
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }

    return str;
}

/* =============================================================================
 * Base64 Functions
 * =============================================================================
 */

/** Base64 alphabet */
static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/**
 * @brief Base64 encode data
 */
ssize_t base64_encode(const uint8_t *data, size_t data_len,
                     char *encoded_out, size_t encoded_out_size)
{
    size_t i;
    uint32_t octet_a, octet_b, octet_c;
    uint32_t triple;
    size_t encoded_len = 0;

    if (!data || !encoded_out || encoded_out_size == 0) {
        return -1;
    }

    for (i = 0; i < data_len; i += 3) {
        octet_a = (unsigned char)data[i];
        octet_b = i + 1 < data_len ? (unsigned char)data[i + 1] : 0;
        octet_c = i + 2 < data_len ? (unsigned char)data[i + 2] : 0;

        triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        if (encoded_len + 4 > encoded_out_size) {
            return -1;
        }

        encoded_out[encoded_len++] = base64_chars[(triple >> 18) & 0x3F];
        encoded_out[encoded_len++] = base64_chars[(triple >> 12) & 0x3F];
        encoded_out[encoded_len++] = i + 1 < data_len ?
                                      base64_chars[(triple >> 6) & 0x3F] : '=';
        encoded_out[encoded_len++] = i + 2 < data_len ?
                                      base64_chars[triple & 0x3F] : '=';
    }

    if (encoded_len >= encoded_out_size) {
        return -1;
    }

    encoded_out[encoded_len] = '\0';
    return (ssize_t)encoded_len;
}

/**
 * @brief Base64 decode data
 */
ssize_t base64_decode(const char *encoded, uint8_t *data_out,
                     size_t data_out_size)
{
    size_t i = 0;
    size_t j = 0;
    uint32_t sextet_a, sextet_b, sextet_c, sextet_d;
    uint32_t triple;

    if (!encoded || !data_out || data_out_size == 0) {
        return -1;
    }

    while (i < strlen(encoded) && j < data_out_size) {
        /* Get sextets */
        sextet_a = strchr(base64_chars, encoded[i]) ? 
                   strchr(base64_chars, encoded[i]) - base64_chars : 0;
        i++;
        if (i >= strlen(encoded)) return -1;

        sextet_b = strchr(base64_chars, encoded[i]) ?
                   strchr(base64_chars, encoded[i]) - base64_chars : 0;
        i++;

        sextet_c = encoded[i] == '=' ? 0 :
                   strchr(base64_chars, encoded[i]) ?
                   strchr(base64_chars, encoded[i]) - base64_chars : 0;
        i++;

        sextet_d = encoded[i] == '=' ? 0 :
                   strchr(base64_chars, encoded[i]) ?
                   strchr(base64_chars, encoded[i]) - base64_chars : 0;
        i++;

        triple = (sextet_a << 18) | (sextet_b << 12) |
                 (sextet_c << 6) | sextet_d;

        if (j < data_out_size) {
            data_out[j++] = (triple >> 16) & 0xFF;
        }
        if (encoded[i - 2] != '=' && j < data_out_size) {
            data_out[j++] = (triple >> 8) & 0xFF;
        }
        if (encoded[i - 1] != '=' && j < data_out_size) {
            data_out[j++] = triple & 0xFF;
        }
    }

    return (ssize_t)j;
}

/* =============================================================================
 * Cryptographic Functions
 * =============================================================================
 */

/**
 * @brief Calculate SHA1 hash
 */
int sha1_digest(const uint8_t *data, size_t data_len,
               uint8_t *digest_out)
{
    SHA_CTX ctx;

    if (!data || !digest_out) {
        return WS_EINVAL;
    }

    if (!SHA1_Init(&ctx)) {
        return WS_ESSL;
    }

    if (!SHA1_Update(&ctx, data, data_len)) {
        return WS_ESSL;
    }

    if (!SHA1_Final(digest_out, &ctx)) {
        return WS_ESSL;
    }

    return WS_SUCCESS;
}

/**
 * @brief Generate random bytes
 */
int random_bytes(uint8_t *buffer, size_t size)
{
    if (!buffer || size == 0) {
        return WS_EINVAL;
    }

    if (!RAND_bytes(buffer, size)) {
        return WS_ESSL;
    }

    return WS_SUCCESS;
}

/* =============================================================================
 * Socket Functions
 * =============================================================================
 */

/**
 * @brief Create TCP socket
 */
int socket_create_tcp(int blocking)
{
    int sock_fd;
    int flags;

    sock_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_fd < 0) {
        log_error("Failed to create socket: %s", strerror(errno));
        return WS_ESOCKET;
    }

    /* Set socket options */
    int optval = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        log_warn("Failed to set SO_REUSEADDR: %s", strerror(errno));
    }

    if (!blocking) {
        flags = fcntl(sock_fd, F_GETFL, 0);
        if (fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            log_error("Failed to set non-blocking mode: %s", strerror(errno));
            close(sock_fd);
            return WS_ESOCKET;
        }
    }

    return sock_fd;
}

/**
 * @brief Connect socket to host:port
 */
int socket_connect(int sock_fd, const char *host, uint16_t port,
                  int timeout_sec)
{
    struct addrinfo hints, *result, *rp;
    char port_str[6];
    int ret;
    fd_set write_set;
    struct timeval tv;
    int error;
    socklen_t error_len;

    if (!host || port == 0) {
        return WS_EINVAL;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(port_str, sizeof(port_str), "%u", port);

    ret = getaddrinfo(host, port_str, &hints, &result);
    if (ret != 0) {
        log_error("Failed to resolve %s: %s", host, gai_strerror(ret));
        return WS_ESOCKET;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        ret = connect(sock_fd, rp->ai_addr, rp->ai_addrlen);
        if (ret == 0) {
            freeaddrinfo(result);
            return WS_SUCCESS;
        }

        if (errno != EINPROGRESS && errno != EWOULDBLOCK) {
            continue;
        }

        /* Handle timeout for non-blocking connect */
        if (timeout_sec > 0) {
            FD_ZERO(&write_set);
            FD_SET(sock_fd, &write_set);

            tv.tv_sec = timeout_sec;
            tv.tv_usec = 0;

            ret = select(sock_fd + 1, NULL, &write_set, NULL, &tv);
            if (ret <= 0) {
                log_error("Connection timeout to %s:%u", host, port);
                freeaddrinfo(result);
                return WS_ESOCKET;
            }

            /* Check if connection succeeded */
            error_len = sizeof(error);
            if (getsockopt(sock_fd, SOL_SOCKET, SO_ERROR, &error, &error_len) < 0) {
                log_error("getsockopt failed: %s", strerror(errno));
                freeaddrinfo(result);
                return WS_ESOCKET;
            }

            if (error != 0) {
                log_debug("Connection to %s:%u failed: %s", host, port, 
                         strerror(error));
                continue;
            }

            freeaddrinfo(result);
            return WS_SUCCESS;
        }
    }

    log_error("Failed to connect to %s:%u", host, port);
    freeaddrinfo(result);
    return WS_ESOCKET;
}

/**
 * @brief Bind socket to address
 */
int socket_bind(int sock_fd, const char *host, uint16_t port)
{
    struct addrinfo hints, *result, *rp;
    char port_str[6];
    int ret;

    if (!host || port == 0) {
        return WS_EINVAL;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    snprintf(port_str, sizeof(port_str), "%u", port);

    ret = getaddrinfo(host, port_str, &hints, &result);
    if (ret != 0) {
        log_error("Failed to resolve %s: %s", host, gai_strerror(ret));
        return WS_ESOCKET;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        ret = bind(sock_fd, rp->ai_addr, rp->ai_addrlen);
        if (ret == 0) {
            freeaddrinfo(result);
            return WS_SUCCESS;
        }
    }

    log_error("Failed to bind to %s:%u: %s", host, port, strerror(errno));
    freeaddrinfo(result);
    return WS_ESOCKET;
}

/**
 * @brief Set socket to listening state
 */
int socket_listen(int sock_fd, int backlog)
{
    if (listen(sock_fd, backlog) < 0) {
        log_error("listen() failed: %s", strerror(errno));
        return WS_ESOCKET;
    }

    return WS_SUCCESS;
}

/**
 * @brief Accept incoming connection
 */
int socket_accept(int listen_fd, struct sockaddr_storage *addr_out,
                 socklen_t *addr_len_out)
{
    int conn_fd;
    socklen_t addr_len = sizeof(struct sockaddr_storage);

    if (!addr_out || !addr_len_out) {
        return WS_EINVAL;
    }

    conn_fd = accept(listen_fd, (struct sockaddr *)addr_out, &addr_len);
    if (conn_fd < 0) {
        if (errno != EINTR && errno != EAGAIN) {
            log_error("accept() failed: %s", strerror(errno));
        }
        return WS_ESOCKET;
    }

    *addr_len_out = addr_len;
    return conn_fd;
}

/**
 * @brief Send data on socket
 */
ssize_t socket_send(int sock_fd, const uint8_t *data, size_t data_len,
                   int flags)
{
    ssize_t ret;

    if (!data || data_len == 0) {
        return 0;
    }

    ret = send(sock_fd, data, data_len, flags | MSG_NOSIGNAL);
    if (ret < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EPIPE) {
            log_debug("send() error: %s", strerror(errno));
        }
    }

    return ret;
}

/**
 * @brief Receive data from socket
 */
ssize_t socket_recv(int sock_fd, uint8_t *buffer, size_t buffer_size,
                   int flags)
{
    ssize_t ret;

    if (!buffer || buffer_size == 0) {
        return WS_EINVAL;
    }

    ret = recv(sock_fd, buffer, buffer_size, flags);
    if (ret < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            log_debug("recv() error: %s", strerror(errno));
        }
    }

    return ret;
}

/**
 * @brief Shutdown socket
 */
int socket_shutdown(int sock_fd, int how)
{
    if (shutdown(sock_fd, how) < 0) {
        if (errno != ENOTCONN) {
            log_debug("shutdown() failed: %s", strerror(errno));
        }
        return WS_ESOCKET;
    }

    return WS_SUCCESS;
}

/**
 * @brief Close socket
 */
int socket_close(int sock_fd)
{
    if (close(sock_fd) < 0) {
        log_error("close() failed: %s", strerror(errno));
        return WS_ESOCKET;
    }

    return WS_SUCCESS;
}

/**
 * @brief Set socket to non-blocking
 */
int socket_set_nonblocking(int sock_fd)
{
    int flags;

    flags = fcntl(sock_fd, F_GETFL, 0);
    if (flags < 0) {
        log_error("fcntl(F_GETFL) failed: %s", strerror(errno));
        return WS_ESOCKET;
    }

    if (fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        log_error("fcntl(F_SETFL) failed: %s", strerror(errno));
        return WS_ESOCKET;
    }

    return WS_SUCCESS;
}

/**
 * @brief Set socket timeout
 */
int socket_set_timeout(int sock_fd, int timeout_sec)
{
    struct timeval tv;

    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    if (setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        log_error("setsockopt(SO_RCVTIMEO) failed: %s", strerror(errno));
        return WS_ESOCKET;
    }

    if (setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        log_error("setsockopt(SO_SNDTIMEO) failed: %s", strerror(errno));
        return WS_ESOCKET;
    }

    return WS_SUCCESS;
}

/**
 * @brief Set TCP_NODELAY option
 */
int socket_set_nodelay(int sock_fd, int enabled)
{
    int optval = enabled ? 1 : 0;

    if (setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval)) < 0) {
        log_error("setsockopt(TCP_NODELAY) failed: %s", strerror(errno));
        return WS_ESOCKET;
    }

    return WS_SUCCESS;
}

/**
 * @brief Set SO_KEEPALIVE option
 */
int socket_set_keepalive(int sock_fd, int enabled)
{
    int optval = enabled ? 1 : 0;

    if (setsockopt(sock_fd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) < 0) {
        log_error("setsockopt(SO_KEEPALIVE) failed: %s", strerror(errno));
        return WS_ESOCKET;
    }

    return WS_SUCCESS;
}

/**
 * @brief Format address to string
 */
int socket_addr_to_string(const struct sockaddr_storage *addr,
                         socklen_t addr_len, char *str_out,
                         size_t str_size)
{
    int ret;

    if (!addr || !str_out || str_size == 0) {
        return WS_EINVAL;
    }

    ret = getnameinfo((struct sockaddr *)addr, addr_len, str_out, str_size,
                     NULL, 0, NI_NUMERICHOST | NI_NUMERICSERV);
    if (ret != 0) {
        log_error("getnameinfo() failed: %s", gai_strerror(ret));
        return WS_ESOCKET;
    }

    return WS_SUCCESS;
}

/* =============================================================================
 * Buffer Management
 * =============================================================================
 */

/**
 * @brief Initialize circular buffer
 */
int buffer_init(ws_buffer_t *buf, size_t capacity)
{
    if (!buf || capacity == 0) {
        return WS_EINVAL;
    }

    buf->data = (uint8_t *)malloc(capacity);
    if (!buf->data) {
        return WS_ENOMEM;
    }

    buf->capacity = capacity;
    buf->read_pos = 0;
    buf->write_pos = 0;

    if (pthread_mutex_init(&buf->lock, NULL) != 0) {
        free(buf->data);
        return WS_ERROR;
    }

    return WS_SUCCESS;
}

/**
 * @brief Destroy circular buffer
 */
void buffer_destroy(ws_buffer_t *buf)
{
    if (!buf) {
        return;
    }

    pthread_mutex_destroy(&buf->lock);
    if (buf->data) {
        free(buf->data);
        buf->data = NULL;
    }

    buf->capacity = 0;
    buf->read_pos = 0;
    buf->write_pos = 0;
}

/**
 * @brief Write data to buffer
 */
ssize_t buffer_write(ws_buffer_t *buf, const uint8_t *data, size_t len)
{
    size_t available;
    size_t to_write;
    size_t write_end;

    if (!buf || !data || len == 0) {
        return 0;
    }

    pthread_mutex_lock(&buf->lock);

    /* Calculate available space */
    if (buf->write_pos >= buf->read_pos) {
        available = buf->capacity - (buf->write_pos - buf->read_pos);
    } else {
        available = buf->read_pos - buf->write_pos;
    }

    if (available <= 1) {
        pthread_mutex_unlock(&buf->lock);
        return -1;  /* Buffer full */
    }

    to_write = (len < available - 1) ? len : (available - 1);

    /* Handle wrap-around */
    write_end = buf->write_pos + to_write;
    if (write_end > buf->capacity) {
        size_t first_part = buf->capacity - buf->write_pos;
        memcpy(buf->data + buf->write_pos, data, first_part);
        memcpy(buf->data, data + first_part, to_write - first_part);
    } else {
        memcpy(buf->data + buf->write_pos, data, to_write);
    }

    buf->write_pos = (buf->write_pos + to_write) % buf->capacity;

    pthread_mutex_unlock(&buf->lock);

    return (ssize_t)to_write;
}

/**
 * @brief Read data from buffer
 */
ssize_t buffer_read(ws_buffer_t *buf, uint8_t *data, size_t len)
{
    size_t available;
    size_t to_read;
    size_t read_end;

    if (!buf || !data || len == 0) {
        return 0;
    }

    pthread_mutex_lock(&buf->lock);

    /* Calculate available data */
    if (buf->write_pos >= buf->read_pos) {
        available = buf->write_pos - buf->read_pos;
    } else {
        available = buf->capacity - (buf->read_pos - buf->write_pos);
    }

    to_read = (len < available) ? len : available;

    if (to_read == 0) {
        pthread_mutex_unlock(&buf->lock);
        return 0;
    }

    /* Handle wrap-around */
    read_end = buf->read_pos + to_read;
    if (read_end > buf->capacity) {
        size_t first_part = buf->capacity - buf->read_pos;
        memcpy(data, buf->data + buf->read_pos, first_part);
        memcpy(data + first_part, buf->data, to_read - first_part);
    } else {
        memcpy(data, buf->data + buf->read_pos, to_read);
    }

    buf->read_pos = (buf->read_pos + to_read) % buf->capacity;

    pthread_mutex_unlock(&buf->lock);

    return (ssize_t)to_read;
}

/**
 * @brief Get number of bytes available in buffer
 */
size_t buffer_available(const ws_buffer_t *buf)
{
    size_t available;

    if (!buf) {
        return 0;
    }

    pthread_mutex_lock((pthread_mutex_t *)&buf->lock);

    if (buf->write_pos >= buf->read_pos) {
        available = buf->write_pos - buf->read_pos;
    } else {
        available = buf->capacity - (buf->read_pos - buf->write_pos);
    }

    pthread_mutex_unlock((pthread_mutex_t *)&buf->lock);

    return available;
}

/**
 * @brief Check if buffer is empty
 */
int buffer_is_empty(const ws_buffer_t *buf)
{
    if (!buf) {
        return 1;
    }

    return buf->read_pos == buf->write_pos ? 1 : 0;
}

/**
 * @brief Clear buffer
 */
void buffer_clear(ws_buffer_t *buf)
{
    if (!buf) {
        return;
    }

    pthread_mutex_lock(&buf->lock);
    buf->read_pos = 0;
    buf->write_pos = 0;
    pthread_mutex_unlock(&buf->lock);
}
