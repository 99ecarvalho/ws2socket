/**
 * @file common.h
 * @brief Common definitions and data structures for ws2socket
 * @author Eduardo Correia <ecorreia@apliant.com.br>
 * 
 * This file contains common definitions, error codes, and shared data
 * structures used throughout the ws2socket project.
 * 
 * @copyright Copyright (c) 2026 Eduardo Correia <ecorreia@apliant.com.br>
 *
 * This file is part of ws2socket. It is free software, licensed under the
 * GNU Lesser General Public License v3.0 or later. See COPYING.LESSER
 * and COPYING for details.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef WS2SOCKET_COMMON_H
#define WS2SOCKET_COMMON_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

/**
 * @defgroup ErrorCodes Error Codes
 * @{
 */

/** Success */
#define WS_SUCCESS 0
/** Generic error */
#define WS_ERROR -1
/** Out of memory */
#define WS_ENOMEM -2
/** Invalid argument */
#define WS_EINVAL -3
/** Connection refused */
#define WS_ECONNREF -4
/** Socket error */
#define WS_ESOCKET -5
/** SSL/TLS error */
#define WS_ESSL -6
/** Protocol error */
#define WS_EPROTO -7
/** Authentication error */
#define WS_EAUTH -8
/** Internal error */
#define WS_EINTERNAL -9

/** @} */

/**
 * @defgroup Constants Common Constants
 * @{
 */

/** Program version (normally set by the build system from CMakeLists.txt) */
#ifndef WS2SOCKET_VERSION
#define WS2SOCKET_VERSION "0.1.0"
#endif

/** Maximum buffer size for frames */
#define MAX_FRAME_SIZE 65536

/** Default maximum size of a message received from a WebSocket client */
#define DEFAULT_MAX_MESSAGE_SIZE (1024 * 1024)
/** Default HTTP port */
#define DEFAULT_HTTP_PORT 80
/** Default HTTPS port */
#define DEFAULT_HTTPS_PORT 443
/** Default WebSocket listen port */
#define DEFAULT_WS_PORT 6080
/** Maximum number of concurrent connections */
#define MAX_CONNECTIONS 1024
/** Socket timeout in seconds */
#define SOCKET_TIMEOUT 60
/** Maximum header size */
#define MAX_HEADER_SIZE 4096

/** @} */

/**
 * @defgroup WebSocketStates WebSocket Protocol States
 * @{
 */

/** WebSocket state: new/unconnected */
#define WS_STATE_NEW 0
/** WebSocket state: connecting */
#define WS_STATE_CONNECTING 1
/** WebSocket state: open/connected */
#define WS_STATE_OPEN 2
/** WebSocket state: closing */
#define WS_STATE_CLOSING 3
/** WebSocket state: closed */
#define WS_STATE_CLOSED 4

/** @} */

/**
 * @defgroup WebSocketOpcodes WebSocket Frame Opcodes
 * @{
 */

/** Continuation frame */
#define WS_OPCODE_CONTINUATION 0x0
/** Text frame */
#define WS_OPCODE_TEXT 0x1
/** Binary frame */
#define WS_OPCODE_BINARY 0x2
/** Connection close */
#define WS_OPCODE_CLOSE 0x8
/** Ping */
#define WS_OPCODE_PING 0x9
/** Pong */
#define WS_OPCODE_PONG 0xA

/** @} */

/**
 * @struct ws_buffer
 * @brief Circular buffer for WebSocket frame data
 * 
 * This structure manages a circular buffer used for queuing
 * WebSocket frames to be sent or received.
 */
typedef struct {
    /** Buffer data */
    uint8_t *data;
    /** Current read position */
    size_t read_pos;
    /** Current write position */
    size_t write_pos;
    /** Total buffer capacity */
    size_t capacity;
    /** Mutex for thread safety */
    pthread_mutex_t lock;
} ws_buffer_t;

/**
 * @struct sockaddr_any
 * @brief Generic socket address structure
 * 
 * Holds either IPv4 or IPv6 socket address information.
 */
typedef struct {
    /** Address family (AF_INET or AF_INET6) */
    int family;
    /** IPv4 address */
    struct sockaddr_in addr4;
    /** IPv6 address */
    struct sockaddr_in6 addr6;
} sockaddr_any_t;

/**
 * @struct ws_close_frame
 * @brief WebSocket close frame data
 * 
 * Contains close code and reason when a WebSocket connection closes.
 */
typedef struct {
    /** Close code (1000-4999) */
    uint16_t code;
    /** Close reason string (null-terminated) */
    char *reason;
    /** Length of reason string */
    size_t reason_len;
} ws_close_frame_t;

/**
 * @struct ws_frame_header
 * @brief WebSocket frame header information
 * 
 * Represents the parsed header of a WebSocket frame.
 */
typedef struct {
    /** FIN flag: final fragment */
    uint8_t fin;
    /** Reserved bits (extensions) */
    uint8_t rsv;
    /** Opcode (see @ref WebSocketOpcodes) */
    uint8_t opcode;
    /** Mask flag: frame is masked */
    uint8_t mask;
    /** Payload length */
    uint64_t payload_len;
    /** Masking key (4 bytes) */
    uint8_t mask_key[4];
} ws_frame_header_t;

#endif /* WS2SOCKET_COMMON_H */
