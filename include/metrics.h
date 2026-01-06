/**
 * @file metrics.h
 * @brief Metrics Collection and Export
 * @author Eduardo Correia <ecorreia@apliant.com.br>
 * 
 * Collects and exports metrics in Prometheus format.
 * 
 * License: LGPL v3
 */

#ifndef WS2SOCKET_METRICS_H
#define WS2SOCKET_METRICS_H

#include "common.h"

/**
 * @struct metrics
 * @brief Metrics collector
 */
typedef struct {
    /* Connection metrics */
    uint64_t total_connections;
    uint64_t active_connections;
    uint64_t failed_connections;
    uint64_t total_websocket_upgrades;
    
    /* Data transfer metrics */
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t frames_sent;
    uint64_t frames_received;
    
    /* HTTP metrics */
    uint64_t http_requests;
    uint64_t http_200;
    uint64_t http_404;
    uint64_t http_500;
    
    /* Authentication metrics */
    uint64_t auth_success;
    uint64_t auth_failure;
    
    /* Pool metrics */
    uint64_t pool_hits;
    uint64_t pool_misses;
    
    /* Timing metrics (microseconds) */
    uint64_t handshake_time_total;
    uint64_t handshake_count;
    
    /* Server info */
    time_t start_time;
    uint32_t uptime_seconds;
    
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
 */
void metrics_inc_connections(metrics_t *m);
void metrics_dec_connections(metrics_t *m);
void metrics_inc_failed_connections(metrics_t *m);

/**
 * @brief Increment data transfer
 */
void metrics_add_bytes_sent(metrics_t *m, uint64_t bytes);
void metrics_add_bytes_received(metrics_t *m, uint64_t bytes);

/**
 * @brief Increment frame counters
 */
void metrics_inc_frames_sent(metrics_t *m);
void metrics_inc_frames_received(metrics_t *m);

/**
 * @brief Increment HTTP counters
 */
void metrics_inc_http_request(metrics_t *m, int status_code);

/**
 * @brief Increment authentication counters
 */
void metrics_inc_auth_success(metrics_t *m);
void metrics_inc_auth_failure(metrics_t *m);

/**
 * @brief Record handshake time
 */
void metrics_record_handshake_time(metrics_t *m, uint64_t microseconds);

/**
 * @brief Export metrics in Prometheus format
 * 
 * @param m Pointer to metrics_t
 * @param buffer Output buffer
 * @param buffer_size Size of buffer
 * @return Number of bytes written
 */
ssize_t metrics_export_prometheus(metrics_t *m, char *buffer, size_t buffer_size);

/**
 * @brief Export metrics in JSON format
 * 
 * @param m Pointer to metrics_t
 * @param buffer Output buffer
 * @param buffer_size Size of buffer
 * @return Number of bytes written
 */
ssize_t metrics_export_json(metrics_t *m, char *buffer, size_t buffer_size);

/**
 * @brief Get current uptime
 */
uint32_t metrics_get_uptime(metrics_t *m);

#endif /* WS2SOCKET_METRICS_H */
