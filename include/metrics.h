/**
 * @file metrics.h
 * @brief Metrics Collection and Export
 * @author Eduardo Correia <ecorreia@apliant.com.br>
 *
 * Collects process-wide counters and exports them in the Prometheus text
 * format. ws2socket handles each client in its own process, so the counters
 * live in an anonymous shared memory mapping created before the first fork
 * and are updated with lock-free atomic operations. The update functions are
 * async-signal-safe and do nothing until metrics_init() has been called.
 *
 * @copyright Copyright (c) 2026 Eduardo Correia <ecorreia@apliant.com.br>
 *
 * This file is part of ws2socket. It is free software, licensed under the
 * GNU Lesser General Public License v3.0 or later. See COPYING.LESSER
 * and COPYING for details.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef WS2SOCKET_METRICS_H
#define WS2SOCKET_METRICS_H

#include "common.h"

/**
 * @brief Counter type
 *
 * 64-bit where the CPU supports lock-free 64-bit atomics, otherwise the
 * native word size. Lock-free operations are required because counters are
 * shared between processes and updated from a signal handler.
 */
#if defined(__GCC_ATOMIC_LLONG_LOCK_FREE) && __GCC_ATOMIC_LLONG_LOCK_FREE == 2
typedef uint64_t metric_counter_t;
#else
typedef unsigned long metric_counter_t;
#endif

/**
 * @brief Reasons a client was turned away
 */
typedef enum {
    METRIC_REJECT_LIMIT = 0,   /**< max_connections reached (503) */
    METRIC_REJECT_AUTH,        /**< missing or wrong credentials (401) */
    METRIC_REJECT_TOKEN,       /**< missing or unknown token (403) */
    METRIC_REJECT_TARGET,      /**< target unreachable (502) */
    METRIC_REJECT_TLS,         /**< TLS handshake failed */
    METRIC_REJECT_COUNT
} metrics_reject_t;

/** Number of distinct HTTP status codes tracked individually */
#define METRICS_HTTP_CODES 17

/**
 * @struct metrics
 * @brief Shared metrics counters
 */
typedef struct {
    /** Accepted TCP connections */
    metric_counter_t connections_total;
    /** Connections currently being handled (live child processes) */
    metric_counter_t connections_active;
    /** Clients turned away, by reason */
    metric_counter_t rejected[METRIC_REJECT_COUNT];

    /** WebSocket sessions established */
    metric_counter_t ws_sessions_total;
    /** WebSocket sessions currently open */
    metric_counter_t ws_sessions_active;
    /** Sum of the durations of finished sessions, in seconds */
    metric_counter_t ws_session_seconds;

    /** Payload bytes forwarded from clients to targets */
    metric_counter_t bytes_to_target;
    /** Payload bytes forwarded from targets to clients */
    metric_counter_t bytes_to_client;
    /** WebSocket messages received from clients */
    metric_counter_t messages_from_client;
    /** WebSocket messages sent to clients */
    metric_counter_t messages_to_client;

    /** HTTP responses by status code (see metrics.c for the code list) */
    metric_counter_t http_responses[METRICS_HTTP_CODES];
    /** HTTP responses with any other status code */
    metric_counter_t http_responses_other;

    /** Successful HTTP authentications */
    metric_counter_t auth_success;
    /** Failed HTTP authentications (wrong user or password) */
    metric_counter_t auth_failure;

    /** Process start time (Unix time) */
    time_t start_time;
} metrics_t;

/**
 * @brief Create the shared metrics area
 *
 * Must be called in the parent before any fork(). Calling it again is a
 * no-op.
 *
 * @return WS_SUCCESS, or WS_ENOMEM if the mapping could not be created
 */
int metrics_init(void);

/**
 * @brief Get the shared metrics area
 *
 * @return Pointer to the counters, or NULL before metrics_init()
 */
metrics_t *metrics_get(void);

/** @brief Count an accepted connection (parent, at fork time) */
void metrics_connection_opened(void);

/** @brief Count a finished connection (async-signal-safe; SIGCHLD handler) */
void metrics_connection_closed(void);

/**
 * @brief Count a rejected client
 * @param reason Why the client was turned away
 */
void metrics_rejected(metrics_reject_t reason);

/** @brief Count a newly established WebSocket session */
void metrics_ws_session_started(void);

/**
 * @brief Count a finished WebSocket session
 * @param seconds How long the session lasted
 */
void metrics_ws_session_ended(uint64_t seconds);

/**
 * @brief Count a message forwarded from a client to its target
 * @param bytes Payload size
 */
void metrics_add_to_target(uint64_t bytes);

/**
 * @brief Count data forwarded from a target to its client
 * @param bytes Payload size
 */
void metrics_add_to_client(uint64_t bytes);

/**
 * @brief Count an HTTP response
 * @param status_code Status code that was sent
 */
void metrics_http_response(int status_code);

/**
 * @brief Count an authentication attempt that supplied credentials
 * @param success Non-zero if the credentials were valid
 */
void metrics_auth(int success);

/**
 * @brief Export all metrics in the Prometheus text exposition format
 *
 * @param buffer Output buffer
 * @param buffer_size Size of the output buffer
 * @return Number of bytes written, or -1 if the buffer is too small or
 *         metrics are not initialized
 */
ssize_t metrics_export_prometheus(char *buffer, size_t buffer_size);

#endif /* WS2SOCKET_METRICS_H */
