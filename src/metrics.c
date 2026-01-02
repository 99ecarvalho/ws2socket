/**
 * @file metrics.c
 * @brief Metrics Collection Implementation
 */

#include "metrics.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

metrics_t *metrics_init(void)
{
    metrics_t *m = (metrics_t *)calloc(1, sizeof(metrics_t));
    if (!m) return NULL;
    
    m->start_time = time(NULL);
    pthread_mutex_init(&m->lock, NULL);
    
    return m;
}

void metrics_destroy(metrics_t *m)
{
    if (!m) return;
    pthread_mutex_destroy(&m->lock);
    free(m);
}

void metrics_inc_connections(metrics_t *m)
{
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    m->total_connections++;
    m->active_connections++;
    pthread_mutex_unlock(&m->lock);
}

void metrics_dec_connections(metrics_t *m)
{
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    if (m->active_connections > 0) m->active_connections--;
    pthread_mutex_unlock(&m->lock);
}

void metrics_inc_failed_connections(metrics_t *m)
{
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    m->failed_connections++;
    pthread_mutex_unlock(&m->lock);
}

void metrics_add_bytes_sent(metrics_t *m, uint64_t bytes)
{
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    m->bytes_sent += bytes;
    pthread_mutex_unlock(&m->lock);
}

void metrics_add_bytes_received(metrics_t *m, uint64_t bytes)
{
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    m->bytes_received += bytes;
    pthread_mutex_unlock(&m->lock);
}

void metrics_inc_frames_sent(metrics_t *m)
{
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    m->frames_sent++;
    pthread_mutex_unlock(&m->lock);
}

void metrics_inc_frames_received(metrics_t *m)
{
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    m->frames_received++;
    pthread_mutex_unlock(&m->lock);
}

void metrics_inc_http_request(metrics_t *m, int status_code)
{
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    m->http_requests++;
    if (status_code == 200) m->http_200++;
    else if (status_code == 404) m->http_404++;
    else if (status_code >= 500) m->http_500++;
    pthread_mutex_unlock(&m->lock);
}

void metrics_inc_auth_success(metrics_t *m)
{
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    m->auth_success++;
    pthread_mutex_unlock(&m->lock);
}

void metrics_inc_auth_failure(metrics_t *m)
{
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    m->auth_failure++;
    pthread_mutex_unlock(&m->lock);
}

void metrics_record_handshake_time(metrics_t *m, uint64_t microseconds)
{
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    m->handshake_time_total += microseconds;
    m->handshake_count++;
    pthread_mutex_unlock(&m->lock);
}

uint32_t metrics_get_uptime(metrics_t *m)
{
    if (!m) return 0;
    return (uint32_t)(time(NULL) - m->start_time);
}

ssize_t metrics_export_prometheus(metrics_t *m, char *buffer, size_t buffer_size)
{
    if (!m || !buffer || buffer_size == 0) return -1;
    
    pthread_mutex_lock(&m->lock);
    
    m->uptime_seconds = metrics_get_uptime(m);
    
    int written = snprintf(buffer, buffer_size,
        "# HELP ws2socket_connections_total Total connections\n"
        "# TYPE ws2socket_connections_total counter\n"
        "ws2socket_connections_total %lu\n"
        "\n"
        "# HELP ws2socket_connections_active Active connections\n"
        "# TYPE ws2socket_connections_active gauge\n"
        "ws2socket_connections_active %lu\n"
        "\n"
        "# HELP ws2socket_connections_failed Failed connections\n"
        "# TYPE ws2socket_connections_failed counter\n"
        "ws2socket_connections_failed %lu\n"
        "\n"
        "# HELP ws2socket_bytes_sent_total Bytes sent\n"
        "# TYPE ws2socket_bytes_sent_total counter\n"
        "ws2socket_bytes_sent_total %lu\n"
        "\n"
        "# HELP ws2socket_bytes_received_total Bytes received\n"
        "# TYPE ws2socket_bytes_received_total counter\n"
        "ws2socket_bytes_received_total %lu\n"
        "\n"
        "# HELP ws2socket_frames_sent_total WebSocket frames sent\n"
        "# TYPE ws2socket_frames_sent_total counter\n"
        "ws2socket_frames_sent_total %lu\n"
        "\n"
        "# HELP ws2socket_frames_received_total WebSocket frames received\n"
        "# TYPE ws2socket_frames_received_total counter\n"
        "ws2socket_frames_received_total %lu\n"
        "\n"
        "# HELP ws2socket_http_requests_total HTTP requests\n"
        "# TYPE ws2socket_http_requests_total counter\n"
        "ws2socket_http_requests_total %lu\n"
        "\n"
        "# HELP ws2socket_http_status HTTP status codes\n"
        "# TYPE ws2socket_http_status counter\n"
        "ws2socket_http_status{code=\"200\"} %lu\n"
        "ws2socket_http_status{code=\"404\"} %lu\n"
        "ws2socket_http_status{code=\"500\"} %lu\n"
        "\n"
        "# HELP ws2socket_auth_total Authentication attempts\n"
        "# TYPE ws2socket_auth_total counter\n"
        "ws2socket_auth_total{result=\"success\"} %lu\n"
        "ws2socket_auth_total{result=\"failure\"} %lu\n"
        "\n"
        "# HELP ws2socket_uptime_seconds Server uptime\n"
        "# TYPE ws2socket_uptime_seconds gauge\n"
        "ws2socket_uptime_seconds %u\n",
        (unsigned long)m->total_connections,
        (unsigned long)m->active_connections,
        (unsigned long)m->failed_connections,
        (unsigned long)m->bytes_sent,
        (unsigned long)m->bytes_received,
        (unsigned long)m->frames_sent,
        (unsigned long)m->frames_received,
        (unsigned long)m->http_requests,
        (unsigned long)m->http_200,
        (unsigned long)m->http_404,
        (unsigned long)m->http_500,
        (unsigned long)m->auth_success,
        (unsigned long)m->auth_failure,
        (unsigned int)m->uptime_seconds
    );
    
    pthread_mutex_unlock(&m->lock);
    
    return (written >= 0 && (size_t)written < buffer_size) ? written : -1;
}

ssize_t metrics_export_json(metrics_t *m, char *buffer, size_t buffer_size)
{
    if (!m || !buffer || buffer_size == 0) return -1;
    
    pthread_mutex_lock(&m->lock);
    
    m->uptime_seconds = metrics_get_uptime(m);
    
    int written = snprintf(buffer, buffer_size,
        "{\n"
        "  \"connections\": {\n"
        "    \"total\": %lu,\n"
        "    \"active\": %lu,\n"
        "    \"failed\": %lu\n"
        "  },\n"
        "  \"traffic\": {\n"
        "    \"bytes_sent\": %lu,\n"
        "    \"bytes_received\": %lu,\n"
        "    \"frames_sent\": %lu,\n"
        "    \"frames_received\": %lu\n"
        "  },\n"
        "  \"http\": {\n"
        "    \"total_requests\": %lu,\n"
        "    \"status_200\": %lu,\n"
        "    \"status_404\": %lu,\n"
        "    \"status_500\": %lu\n"
        "  },\n"
        "  \"authentication\": {\n"
        "    \"success\": %lu,\n"
        "    \"failure\": %lu\n"
        "  },\n"
        "  \"server\": {\n"
        "    \"uptime_seconds\": %u,\n"
        "    \"start_time\": %ld\n"
        "  }\n"
        "}\n",
        (unsigned long)m->total_connections,
        (unsigned long)m->active_connections,
        (unsigned long)m->failed_connections,
        (unsigned long)m->bytes_sent,
        (unsigned long)m->bytes_received,
        (unsigned long)m->frames_sent,
        (unsigned long)m->frames_received,
        (unsigned long)m->http_requests,
        (unsigned long)m->http_200,
        (unsigned long)m->http_404,
        (unsigned long)m->http_500,
        (unsigned long)m->auth_success,
        (unsigned long)m->auth_failure,
        (unsigned int)m->uptime_seconds,
        (long)m->start_time
    );
    
    pthread_mutex_unlock(&m->lock);
    
    return (written >= 0 && (size_t)written < buffer_size) ? written : -1;
}
