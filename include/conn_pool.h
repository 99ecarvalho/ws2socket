/**
 * @file conn_pool.h
 * @brief Connection Pooling
 * @author WebSocket to TCP Proxy Project
 * @version 0.1.0
 * 
 * Manages a pool of reusable connections to reduce connection overhead.
 * 
 * License: LGPL v3
 */

#ifndef WS2SOCKET_CONN_POOL_H
#define WS2SOCKET_CONN_POOL_H

#include "common.h"

/**
 * @struct conn_pool_entry
 * @brief Single connection in pool
 */
typedef struct conn_pool_entry_s {
    int fd;                          /**< File descriptor */
    char host[256];                  /**< Connected host */
    uint16_t port;                   /**< Connected port */
    time_t last_used;                /**< Last access time */
    int in_use;                      /**< Currently in use flag */
    struct conn_pool_entry_s *next;  /**< Next in list */
} conn_pool_entry_t;

/**
 * @struct conn_pool
 * @brief Connection pool manager
 */
typedef struct {
    conn_pool_entry_t *head;    /**< Head of connection list */
    int max_size;               /**< Maximum pool size */
    int current_size;           /**< Current pool size */
    int idle_timeout;           /**< Idle timeout in seconds */
    pthread_mutex_t lock;       /**< Thread safety */
    
    /* Statistics */
    uint64_t hits;              /**< Pool hits */
    uint64_t misses;            /**< Pool misses */
    uint64_t evictions;         /**< Evicted connections */
} conn_pool_t;

/**
 * @brief Create connection pool
 * 
 * @param max_size Maximum number of pooled connections
 * @param idle_timeout Idle timeout in seconds (0 = no timeout)
 * @return Pointer to conn_pool_t or NULL on error
 */
conn_pool_t *conn_pool_create(int max_size, int idle_timeout);

/**
 * @brief Destroy connection pool
 * 
 * @param pool Pointer to conn_pool_t
 */
void conn_pool_destroy(conn_pool_t *pool);

/**
 * @brief Get connection from pool or create new
 * 
 * @param pool Pointer to conn_pool_t
 * @param host Target hostname
 * @param port Target port
 * @param fd_out Pointer to store file descriptor
 * @return WS_SUCCESS on success, error code otherwise
 */
int conn_pool_get(conn_pool_t *pool, const char *host, uint16_t port, int *fd_out);

/**
 * @brief Return connection to pool
 * 
 * @param pool Pointer to conn_pool_t
 * @param fd File descriptor to return
 * @param keep_alive Keep connection alive (1) or close (0)
 * @return WS_SUCCESS on success
 */
int conn_pool_put(conn_pool_t *pool, int fd, int keep_alive);

/**
 * @brief Clean up idle connections
 * 
 * @param pool Pointer to conn_pool_t
 * @return Number of connections removed
 */
int conn_pool_cleanup(conn_pool_t *pool);

/**
 * @brief Get pool statistics
 * 
 * @param pool Pointer to conn_pool_t
 * @param hits Pointer to store hit count
 * @param misses Pointer to store miss count
 * @param size Pointer to store current size
 */
void conn_pool_stats(conn_pool_t *pool, uint64_t *hits, uint64_t *misses, int *size);

#endif /* WS2SOCKET_CONN_POOL_H */
