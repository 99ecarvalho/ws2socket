/**
 * @file auth.h
 * @brief HTTP Basic authentication
 * @author Eduardo Correia <ecorreia@apliant.com.br>
 *
 * Optional HTTP Basic authentication (RFC 7617) against a password file in
 * the htpasswd format ("user:hash" per line). Hashes are verified with the
 * system crypt(3), so any scheme it supports can be used, for example bcrypt
 * ("htpasswd -B"), SHA-512-crypt ("openssl passwd -6") or yescrypt.
 *
 * @copyright Copyright (c) 2026 Eduardo Correia <ecorreia@apliant.com.br>
 *
 * This file is part of ws2socket. It is free software, licensed under the
 * GNU Lesser General Public License v3.0 or later. See COPYING.LESSER
 * and COPYING for details.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef WS2SOCKET_AUTH_H
#define WS2SOCKET_AUTH_H

#include "common.h"

/** Result of checking a request's credentials */
typedef enum {
    AUTH_OK = 0,       /**< Valid credentials */
    AUTH_MISSING,      /**< No (or not Basic) Authorization header */
    AUTH_INVALID       /**< Credentials supplied but wrong or malformed */
} auth_result_t;

/** Opaque password database */
typedef struct auth_s auth_t;

/**
 * @brief Check whether this build supports authentication
 *
 * @return 1 if built with crypt(3) support, 0 otherwise
 */
int auth_supported(void);

/**
 * @brief Load a password file
 *
 * @param path Password file ("user:hash" lines; '#' starts a comment)
 * @param realm Realm reported to clients in WWW-Authenticate
 * @return Password database, or NULL on error (logged)
 */
auth_t *auth_create(const char *path, const char *realm);

/**
 * @brief Free a password database
 */
void auth_destroy(auth_t *auth);

/**
 * @brief Get the realm reported to clients
 */
const char *auth_realm(const auth_t *auth);

/**
 * @brief Check the value of a request's Authorization header
 *
 * The password file is reloaded if it changed on disk. The last accepted
 * header is remembered, so repeated requests on one connection do not pay
 * for the (deliberately slow) password hash again.
 *
 * @param auth Password database
 * @param authorization Authorization header value, or NULL if absent
 * @param user_out Receives the user name on success (may be NULL)
 * @param user_out_size Size of user_out
 * @return AUTH_OK, AUTH_MISSING or AUTH_INVALID
 */
auth_result_t auth_check(auth_t *auth, const char *authorization,
                         char *user_out, size_t user_out_size);

#endif /* WS2SOCKET_AUTH_H */
