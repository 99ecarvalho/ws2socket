/**
 * @file conn_pool.c
 * @brief Connection Pooling Implementation
 * @author Eduardo Correia <ecorreia@apliant.com.br>
 * 
 * License: LGPL v3
 */

#include "conn_pool.h"
#include "logging.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

conn_pool_t *conn_pool_create(int max_size, int idle_timeout)
{
    conn_pool_t *pool = (conn_pool_t *)calloc(1, sizeof(conn_pool_t));
    if (!pool) return NULL;
    
    pool->max_size = max_size > 0 ? max_size : 50;
    pool->idle_timeout = idle_timeout > 0 ? idle_timeout : 300;
    pthread_mutex_init(&pool->lock, NULL);
    
    log_info("Connection pool created: max=%d, timeout=%ds", pool->max_size, pool->idle_timeout);
    return pool;
}

void conn_pool_destroy(conn_pool_t *pool)
{
    if (!pool) return;
    
    pthread_mutex_lock(&pool->lock);
    
    conn_pool_entry_t *entry = pool->head;
    while (entry) {
        conn_pool_entry_t *next = entry->next;
        if (entry->fd >= 0) close(entry->fd);
        free(entry);
        entry = next;
    }
    
    pthread_mutex_unlock(&pool->lock);
    pthread_mutex_destroy(&pool->lock);
    free(pool);
}

int conn_pool_get(conn_pool_t *pool, const char *host, uint16_t port, int *fd_out)
{
    if (!pool || !host || !fd_out) return WS_EINVAL;
    
    pthread_mutex_lock(&pool->lock);
    
    /* Search for existing connection */
    conn_pool_entry_t *entry = pool->head;
    conn_pool_entry_t *prev __attribute__((unused)) = NULL;
    
    while (entry) {
        if (!entry->in_use && 
            strcmp(entry->host, host) == 0 && 
            entry->port == port) {
            /* Found matching connection */
            entry->in_use = 1;
            entry->last_used = time(NULL);
            *fd_out = entry->fd;
            pool->hits++;
            pthread_mutex_unlock(&pool->lock);
            log_debug("Pool hit: %s:%u (fd=%d)", host, port, *fd_out);
            return WS_SUCCESS;
        }
        prev = entry;
        entry = entry->next;
    }
    
    /* Pool miss - create new connection */
    pool->misses++;
    pthread_mutex_unlock(&pool->lock);
    
    int fd = socket_create_tcp(0);
    if (fd < 0) return WS_ESOCKET;
    
    if (socket_connect(fd, host, port, 0) != WS_SUCCESS) {
        close(fd);
        return WS_ERROR;
    }
    
    *fd_out = fd;
    log_debug("Pool miss: created new connection to %s:%u (fd=%d)", host, port, fd);
    return WS_SUCCESS;
}

int conn_pool_put(conn_pool_t *pool, int fd, int keep_alive)
{
    if (!pool || fd < 0) return WS_EINVAL;
    
    if (!keep_alive) {
        close(fd);
        return WS_SUCCESS;
    }
    
    pthread_mutex_lock(&pool->lock);
    
    /* Find entry or create new */
    conn_pool_entry_t *entry = pool->head;
    while (entry) {
        if (entry->fd == fd) {
            entry->in_use = 0;
            entry->last_used = time(NULL);
            pthread_mutex_unlock(&pool->lock);
            return WS_SUCCESS;
        }
        entry = entry->next;
    }
    
    /* Create new entry if pool not full */
    if (pool->current_size >= pool->max_size) {
        pthread_mutex_unlock(&pool->lock);
        close(fd);
        pool->evictions++;
        return WS_SUCCESS;
    }
    
    entry = (conn_pool_entry_t *)calloc(1, sizeof(conn_pool_entry_t));
    if (entry) {
        entry->fd = fd;
        entry->last_used = time(NULL);
        entry->next = pool->head;
        pool->head = entry;
        pool->current_size++;
    }
    
    pthread_mutex_unlock(&pool->lock);
    return WS_SUCCESS;
}

int conn_pool_cleanup(conn_pool_t *pool)
{
    if (!pool) return 0;
    
    int removed = 0;
    time_t now = time(NULL);
    
    pthread_mutex_lock(&pool->lock);
    
    conn_pool_entry_t *entry = pool->head;
    conn_pool_entry_t *prev = NULL;
    
    while (entry) {
        if (!entry->in_use && (now - entry->last_used) > pool->idle_timeout) {
            /* Remove idle connection */
            conn_pool_entry_t *next = entry->next;
            close(entry->fd);
            free(entry);
            
            if (prev) prev->next = next;
            else pool->head = next;
            
            pool->current_size--;
            pool->evictions++;
            removed++;
            entry = next;
        } else {
            prev = entry;
            entry = entry->next;
        }
    }
    
    pthread_mutex_unlock(&pool->lock);
    
    if (removed > 0) {
        log_info("Cleaned up %d idle connections", removed);
    }
    
    return removed;
}

void conn_pool_stats(conn_pool_t *pool, uint64_t *hits, uint64_t *misses, int *size)
{
    if (!pool) return;
    
    pthread_mutex_lock(&pool->lock);
    if (hits) *hits = pool->hits;
    if (misses) *misses = pool->misses;
    if (size) *size = pool->current_size;
    pthread_mutex_unlock(&pool->lock);
}
