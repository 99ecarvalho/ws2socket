/**
 * @file metrics.h
 * @brief Metrics Collection and Export
 * @author Eduardo Correia <ecorreia@apliant.com.br>
 * 
 * Collects and exports metrics in Prometheus format.
 * 
 * @copyright Copyright (c) 2026 Eduardo Correia <ecorreia@apliant.com.br>
 *
 * This file is part of ws2socket. It is free software, licensed under the
 * GNU Lesser General Public License v3.0 or later. See COPYING for details.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef WS2SOCKET_METRICS_H
#define WS2SOCKET_METRICS_H

#include "common.h"

/**
 * @struct metrics
 * @brief Metrics collector
 * 
 * This structure holds various counters and metrics for the application.
 */
typedef struct {
    /** Total number of connections handled. */
    uint64_t total_connections;
    /** Number of currently active connections. */
    uint64_t active_connections;
    /** Number of failed connection attempts. */
    uint64_t failed_connections;
    /** Total number of WebSocket upgrades performed. */
    uint64_t total_websocket_upgrades;

    /** Total bytes sent by the application. */
    uint64_t bytes_sent;
    /** Total bytes received by the application. */
    uint64_t bytes_received;
    /** Total WebSocket frames sent. */
    uint64_t frames_sent;
    /** Total WebSocket frames received. */
    uint64_t frames_received;

    /** Total HTTP requests handled. */
    uint64_t http_requests;
    /** Number of HTTP 200 responses sent. */
    uint64_t http_200;
    /** Number of HTTP 404 responses sent. */
    uint64_t http_404;
    /** Number of HTTP 500 responses sent. */
    uint64_t http_500;

    /** Number of successful authentication attempts. */
    uint64_t auth_success;
    /** Number of failed authentication attempts. */
    uint64_t auth_failure;

    /** Number of connection pool hits. */
    uint64_t pool_hits;
    /** Number of connection pool misses. */
    uint64_t pool_misses;

    /** Total time spent in WebSocket handshakes (microseconds). */
    uint64_t handshake_time_total;
    /** Total number of WebSocket handshakes performed. */
    uint64_t handshake_count;

    /** Server start time. */
    time_t start_time;
    /** Server uptime in seconds. */
    uint32_t uptime_seconds;

    /** Mutex for thread-safe access to metrics. */
    pthread_mutex_t lock;
} metrics_t;

/**
 * @brief Initialize metrics
 * 
 * @return Pointer to metrics_t or NULL on error
 */
metrics_t *metrics_init(void);

/**
 * @brief Destroy metrics
 * 
 * @param m Pointer to metrics_t
 */
void metrics_destroy(metrics_t *m);

/**
 * @brief Increment connection counter
 * 
 * Increments the total and active connection counters.
 * 
 * @param m Pointer to metrics_t structure.
 */
void metrics_inc_connections(metrics_t *m);

/**
 * @brief Decrement connection counter
 * 
 * Decrements the active connection counter.
 * 
 * @param m Pointer to metrics_t structure.
 */
void metrics_dec_connections(metrics_t *m);

/**
 * @brief Increment failed connection counter
 * 
 * Increments the failed connection counter.
 * 
 * @param m Pointer to metrics_t structure.
 */
void metrics_inc_failed_connections(metrics_t *m);

/**
 * @brief Increment data transfer sent
 * 
 * @param m Pointer to metrics_t
 * @param bytes Number of bytes to add
 */
void metrics_add_bytes_sent(metrics_t *m, uint64_t bytes);

/**
 * @brief Increment data transfer received
 * 
 * @param m Pointer to metrics_t
 * @param bytes Number of bytes to add
 */
void metrics_add_bytes_received(metrics_t *m, uint64_t bytes);

/**
 * @brief Increment frame counters sent
 * 
 * @param m Pointer to metrics_t
 */
void metrics_inc_frames_sent(metrics_t *m);

/**
 * @brief Increment frame counters received
 * 
 * @param m Pointer to metrics_t
 */
void metrics_inc_frames_received(metrics_t *m);

/**
 * @brief Increment HTTP counters
 * 
 * @param m Pointer to metrics_t
 * @param status_code HTTP status code
 */
void metrics_inc_http_request(metrics_t *m, int status_code);

/**
 * @brief Increment authentication counters success
 * 
 * @param m Pointer to metrics_t
 */
void metrics_inc_auth_success(metrics_t *m);

/**
 * @brief Increment authentication counters failure
 * 
 * @param m Pointer to metrics_t
 */
void metrics_inc_auth_failure(metrics_t *m);

/**
 * @brief Record handshake time
 * 
 * @param m Pointer to metrics_t
 * @param microseconds Time spent in handshake (in microseconds)
 */
void metrics_record_handshake_time(metrics_t *m, uint64_t microseconds);

/**
 * @brief Export metrics in Prometheus format
 * 
 * Exports the current metrics in Prometheus text format.
 * 
 * @param m Pointer to metrics_t structure.
 * @param buffer Output buffer to write the metrics.
 * @param buffer_size Size of the output buffer.
 * @return Number of bytes written to the buffer.
 */
ssize_t metrics_export_prometheus(metrics_t *m, char *buffer, size_t buffer_size);

/**
 * @brief Export metrics in JSON format
 * 
 * Exports the current metrics in JSON format.
 * 
 * @param m Pointer to metrics_t structure.
 * @param buffer Output buffer to write the metrics.
 * @param buffer_size Size of the output buffer.
 * @return Number of bytes written to the buffer.
 */
ssize_t metrics_export_json(metrics_t *m, char *buffer, size_t buffer_size);

/**
 * @brief Get current uptime
 * 
 * Calculates and returns the current uptime of the server in seconds.
 * 
 * @param m Pointer to metrics_t structure.
 * @return Server uptime in seconds.
 */
uint32_t metrics_get_uptime(metrics_t *m);

#endif /* WS2SOCKET_METRICS_H */
