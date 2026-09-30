/**
 * @file metrics.c
 * @brief Metrics Collection Implementation
 * @author Eduardo Correia <ecorreia@apliant.com.br>
 *
 * @copyright Copyright (c) 2026 Eduardo Correia <ecorreia@apliant.com.br>
 *
 * This file is part of ws2socket. It is free software, licensed under the
 * GNU Lesser General Public License v3.0 or later. See COPYING.LESSER
 * and COPYING for details.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "metrics.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

/** Shared counters, created by metrics_init() */
static metrics_t *g_metrics = NULL;

/** Status codes counted individually, indexed like metrics_t.http_responses */
static const int g_http_codes[METRICS_HTTP_CODES] = {
    101, 200, 206, 301, 304, 400, 401, 403, 404,
    405, 414, 416, 426, 431, 500, 502, 503
};

static const char *g_reject_names[METRIC_REJECT_COUNT] = {
    "limit", "auth", "token", "target", "tls"
};

#define METRIC_ADD(field, n) \
    __atomic_add_fetch(&(field), (metric_counter_t)(n), __ATOMIC_RELAXED)
#define METRIC_SUB(field, n) \
    __atomic_sub_fetch(&(field), (metric_counter_t)(n), __ATOMIC_RELAXED)
#define METRIC_LOAD(field) \
    ((unsigned long long)__atomic_load_n(&(field), __ATOMIC_RELAXED))

/**
 * @brief Create the shared metrics area
 */
int metrics_init(void)
{
    if (g_metrics) {
        return WS_SUCCESS;
    }

    void *area = mmap(NULL, sizeof(metrics_t), PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (area == MAP_FAILED) {
        return WS_ENOMEM;
    }

    /* Anonymous mappings are zero-filled */
    g_metrics = (metrics_t *)area;
    g_metrics->start_time = time(NULL);
    return WS_SUCCESS;
}

/**
 * @brief Get the shared metrics area
 */
metrics_t *metrics_get(void)
{
    return g_metrics;
}

void metrics_connection_opened(void)
{
    if (!g_metrics) return;
    METRIC_ADD(g_metrics->connections_total, 1);
    METRIC_ADD(g_metrics->connections_active, 1);
}

void metrics_connection_closed(void)
{
    if (!g_metrics) return;
    if (METRIC_LOAD(g_metrics->connections_active) > 0) {
        METRIC_SUB(g_metrics->connections_active, 1);
    }
}

void metrics_rejected(metrics_reject_t reason)
{
    if (!g_metrics || reason >= METRIC_REJECT_COUNT) return;
    METRIC_ADD(g_metrics->rejected[reason], 1);
}

void metrics_ws_session_started(void)
{
    if (!g_metrics) return;
    METRIC_ADD(g_metrics->ws_sessions_total, 1);
    METRIC_ADD(g_metrics->ws_sessions_active, 1);
}

void metrics_ws_session_ended(uint64_t seconds)
{
    if (!g_metrics) return;
    METRIC_ADD(g_metrics->ws_session_seconds, seconds);
    if (METRIC_LOAD(g_metrics->ws_sessions_active) > 0) {
        METRIC_SUB(g_metrics->ws_sessions_active, 1);
    }
}

void metrics_add_to_target(uint64_t bytes)
{
    if (!g_metrics) return;
    METRIC_ADD(g_metrics->bytes_to_target, bytes);
    METRIC_ADD(g_metrics->messages_from_client, 1);
}

void metrics_add_to_client(uint64_t bytes)
{
    if (!g_metrics) return;
    METRIC_ADD(g_metrics->bytes_to_client, bytes);
    METRIC_ADD(g_metrics->messages_to_client, 1);
}

void metrics_http_response(int status_code)
{
    if (!g_metrics) return;
    for (int i = 0; i < METRICS_HTTP_CODES; i++) {
        if (g_http_codes[i] == status_code) {
            METRIC_ADD(g_metrics->http_responses[i], 1);
            return;
        }
    }
    METRIC_ADD(g_metrics->http_responses_other, 1);
}

void metrics_auth(int success)
{
    if (!g_metrics) return;
    if (success) {
        METRIC_ADD(g_metrics->auth_success, 1);
    } else {
        METRIC_ADD(g_metrics->auth_failure, 1);
    }
}

/**
 * @brief Append formatted text to the export buffer
 *
 * @return 0 on success, -1 if the buffer is full
 */
static int emit(char *buffer, size_t size, size_t *pos, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static int emit(char *buffer, size_t size, size_t *pos, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buffer + *pos, size - *pos, fmt, args);
    va_end(args);

    if (n < 0 || (size_t)n >= size - *pos) {
        return -1;
    }
    *pos += (size_t)n;
    return 0;
}

#define EMIT(...) \
    do { if (emit(buffer, buffer_size, &pos, __VA_ARGS__) < 0) return -1; } while (0)

/**
 * @brief Export all metrics in the Prometheus text exposition format
 */
ssize_t metrics_export_prometheus(char *buffer, size_t buffer_size)
{
    metrics_t *m = g_metrics;
    size_t pos = 0;

    if (!m || !buffer || buffer_size == 0) {
        return -1;
    }

    EMIT("# HELP ws2socket_build_info Build information.\n"
         "# TYPE ws2socket_build_info gauge\n"
         "ws2socket_build_info{version=\"%s\"} 1\n",
         WS2SOCKET_VERSION);

    EMIT("# HELP ws2socket_start_time_seconds Start time of the process, in Unix time.\n"
         "# TYPE ws2socket_start_time_seconds gauge\n"
         "ws2socket_start_time_seconds %lld\n",
         (long long)m->start_time);

    EMIT("# HELP ws2socket_connections_total Accepted TCP connections.\n"
         "# TYPE ws2socket_connections_total counter\n"
         "ws2socket_connections_total %llu\n"
         "# HELP ws2socket_connections_active Connections currently being handled.\n"
         "# TYPE ws2socket_connections_active gauge\n"
         "ws2socket_connections_active %llu\n",
         METRIC_LOAD(m->connections_total), METRIC_LOAD(m->connections_active));

    EMIT("# HELP ws2socket_rejected_total Clients turned away, by reason.\n"
         "# TYPE ws2socket_rejected_total counter\n");
    for (int i = 0; i < METRIC_REJECT_COUNT; i++) {
        EMIT("ws2socket_rejected_total{reason=\"%s\"} %llu\n",
             g_reject_names[i], METRIC_LOAD(m->rejected[i]));
    }

    EMIT("# HELP ws2socket_websocket_sessions_total WebSocket sessions established.\n"
         "# TYPE ws2socket_websocket_sessions_total counter\n"
         "ws2socket_websocket_sessions_total %llu\n"
         "# HELP ws2socket_websocket_sessions_active WebSocket sessions currently open.\n"
         "# TYPE ws2socket_websocket_sessions_active gauge\n"
         "ws2socket_websocket_sessions_active %llu\n"
         "# HELP ws2socket_websocket_session_seconds_total Total duration of finished sessions.\n"
         "# TYPE ws2socket_websocket_session_seconds_total counter\n"
         "ws2socket_websocket_session_seconds_total %llu\n",
         METRIC_LOAD(m->ws_sessions_total), METRIC_LOAD(m->ws_sessions_active),
         METRIC_LOAD(m->ws_session_seconds));

    EMIT("# HELP ws2socket_bytes_total Payload bytes forwarded, by direction.\n"
         "# TYPE ws2socket_bytes_total counter\n"
         "ws2socket_bytes_total{direction=\"to_target\"} %llu\n"
         "ws2socket_bytes_total{direction=\"to_client\"} %llu\n"
         "# HELP ws2socket_messages_total WebSocket messages forwarded, by direction.\n"
         "# TYPE ws2socket_messages_total counter\n"
         "ws2socket_messages_total{direction=\"to_target\"} %llu\n"
         "ws2socket_messages_total{direction=\"to_client\"} %llu\n",
         METRIC_LOAD(m->bytes_to_target), METRIC_LOAD(m->bytes_to_client),
         METRIC_LOAD(m->messages_from_client), METRIC_LOAD(m->messages_to_client));

    EMIT("# HELP ws2socket_http_responses_total HTTP responses, by status code.\n"
         "# TYPE ws2socket_http_responses_total counter\n");
    for (int i = 0; i < METRICS_HTTP_CODES; i++) {
        EMIT("ws2socket_http_responses_total{code=\"%d\"} %llu\n",
             g_http_codes[i], METRIC_LOAD(m->http_responses[i]));
    }
    EMIT("ws2socket_http_responses_total{code=\"other\"} %llu\n",
         METRIC_LOAD(m->http_responses_other));

    EMIT("# HELP ws2socket_auth_total Authentication attempts with credentials, by result.\n"
         "# TYPE ws2socket_auth_total counter\n"
         "ws2socket_auth_total{result=\"success\"} %llu\n"
         "ws2socket_auth_total{result=\"failure\"} %llu\n",
         METRIC_LOAD(m->auth_success), METRIC_LOAD(m->auth_failure));

    return (ssize_t)pos;
}
