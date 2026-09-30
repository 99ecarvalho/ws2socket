/**
 * @file conn_pool.c
 * @brief Connection Pooling Implementation
 * @author Eduardo Correia <ecorreia@apliant.com.br>
 * 
 * @copyright Copyright (c) 2026 Eduardo Correia <ecorreia@apliant.com.br>
 *
 * This file is part of ws2socket. It is free software, licensed under the
 * GNU Lesser General Public License v3.0 or later. See COPYING for details.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "conn_pool.h"
#include "logging.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/**
 * @brief Create a connection pool
 * 
 * Allocates and initializes a connection pool with the specified maximum size
 * and idle timeout.
 * 
 * @param max_size Maximum number of connections in the pool.
 * @param idle_timeout Timeout for idle connections in seconds.
 * @return Pointer to the created connection pool, or NULL on failure.
 */
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

/**
 * @brief Destroy a connection pool
 * 
 * Frees all resources associated with the connection pool.
 * 
 * @param pool Pointer to the connection pool to destroy.
 */
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

/**
 * @brief Get a connection from the pool
 * 
 * Retrieves an existing connection from the pool or creates a new one if none
 * are available.
 * 
 * @param pool Pointer to the connection pool.
 * @param host Hostname for the connection.
 * @param port Port number for the connection.
 * @param fd_out Pointer to store the file descriptor of the connection.
 * @return WS_SUCCESS on success, or an error code on failure.
 */
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

/**
 * @brief Return a connection to the pool
 * 
 * Marks a connection as available for reuse or closes it if keep_alive is false.
 * 
 * @param pool Pointer to the connection pool.
 * @param fd File descriptor of the connection.
 * @param keep_alive Whether to keep the connection alive.
 * @return WS_SUCCESS on success, or an error code on failure.
 */
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

/**
 * @brief Clean up idle connections
 * 
 * Removes idle connections from the pool that have exceeded the idle timeout.
 * 
 * @param pool Pointer to the connection pool.
 * @return Number of connections removed.
 */
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

/**
 * @brief Get connection pool statistics
 * 
 * Retrieves statistics about the connection pool, including hits, misses, and
 * current size.
 * 
 * @param pool Pointer to the connection pool.
 * @param hits Pointer to store the number of pool hits.
 * @param misses Pointer to store the number of pool misses.
 * @param size Pointer to store the current pool size.
 */
void conn_pool_stats(conn_pool_t *pool, uint64_t *hits, uint64_t *misses, int *size)
{
    if (!pool) return;
    
    pthread_mutex_lock(&pool->lock);
    if (hits) *hits = pool->hits;
    if (misses) *misses = pool->misses;
    if (size) *size = pool->current_size;
    pthread_mutex_unlock(&pool->lock);
}
