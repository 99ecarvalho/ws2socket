/**
 * @file token_auth.h
 * @brief Token-based Authentication
 * @author Eduardo Correia <ecorreia@apliant.com.br>
 * 
 * Provides token-based authentication similar to websockify Python implementation.
 * Supports token files with format: token: host:port
 * 
 * @copyright Copyright (c) 2026 Eduardo Correia <ecorreia@apliant.com.br>
 *
 * This file is part of ws2socket. It is free software, licensed under the
 * GNU Lesser General Public License v3.0 or later. See COPYING for details.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef WS2SOCKET_TOKEN_AUTH_H
#define WS2SOCKET_TOKEN_AUTH_H

#include "common.h"

/**
 * @struct token_target
 * @brief Target server for a token
 */
typedef struct {
    char host[256];      /**< Target hostname */
    uint16_t port;       /**< Target port */
} token_target_t;

/**
 * @struct token_auth
 * @brief Token authentication manager
 */
typedef struct token_auth_s {
    char token_file[512];           /**< Path to token file or directory */
    void *tokens;                   /**< Hash table of tokens (internal) */
    time_t last_load;               /**< Last load timestamp */
    int auto_reload;                /**< Auto-reload on file change */
    pthread_mutex_t lock;           /**< Thread safety */
} token_auth_t;

/**
 * @brief Create token authentication manager
 * 
 * @param token_file Path to token file or directory
 * @param auto_reload Enable automatic reload on file changes
 * @return Pointer to token_auth_t or NULL on error
 */
token_auth_t *token_auth_create(const char *token_file, int auto_reload);

/**
 * @brief Destroy token authentication manager
 * 
 * @param auth Pointer to token_auth_t
 */
void token_auth_destroy(token_auth_t *auth);

/**
 * @brief Load/reload tokens from file
 * 
 * @param auth Pointer to token_auth_t
 * @return WS_SUCCESS on success, error code otherwise
 */
int token_auth_load(token_auth_t *auth);

/**
 * @brief Lookup target for token
 * 
 * @param auth Pointer to token_auth_t
 * @param token Token string to lookup
 * @param target_out Pointer to store target info
 * @return WS_SUCCESS if token found, WS_EAUTH if not found or invalid
 */
int token_auth_lookup(token_auth_t *auth, const char *token, token_target_t *target_out);

/**
 * @brief Extract token from WebSocket path
 * 
 * Supports formats:
 *   /websockify?token=TOKEN
 *   /TOKEN/
 *   /?token=TOKEN
 * 
 * @param path HTTP request path
 * @param token_out Buffer to store extracted token
 * @param token_out_size Size of token buffer
 * @return WS_SUCCESS if token extracted, WS_ERROR otherwise
 */
int token_auth_extract_from_path(const char *path, char *token_out, size_t token_out_size);

#endif /* WS2SOCKET_TOKEN_AUTH_H */
