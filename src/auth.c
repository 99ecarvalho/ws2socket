/**
 * @file auth.c
 * @brief HTTP Basic authentication
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

#include "auth.h"
#include "logging.h"
#include "utils.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <openssl/crypto.h>

#ifdef WS2SOCKET_HAVE_CRYPT
#include <crypt.h>
#endif

/** Longest accepted user name, password and hash */
#define AUTH_MAX_FIELD 256

/** One "user:hash" entry */
typedef struct {
    char user[AUTH_MAX_FIELD];
    char hash[AUTH_MAX_FIELD];
} auth_entry_t;

struct auth_s {
    char path[512];
    char realm[128];
    auth_entry_t *entries;
    size_t count;
    time_t mtime;           /**< Modification time of the loaded file */
    char last_ok[768];      /**< Last accepted Authorization header */
};

int auth_supported(void)
{
#ifdef WS2SOCKET_HAVE_CRYPT
    return 1;
#else
    return 0;
#endif
}

/**
 * @brief (Re)load the password file
 */
static int auth_load(auth_t *auth)
{
    FILE *fp;
    char line[1024];
    int line_num = 0;
    struct stat st;
    auth_entry_t *entries = NULL;
    size_t count = 0, capacity = 0;

    fp = fopen(auth->path, "r");
    if (!fp) {
        log_error("Failed to open password file '%s': %s", auth->path, strerror(errno));
        return WS_ERROR;
    }
    if (fstat(fileno(fp), &st) == 0) {
        auth->mtime = st.st_mtime;
        if (st.st_mode & (S_IRWXG | S_IRWXO)) {
            log_warn("Password file '%s' is accessible by other users; "
                     "consider chmod 600", auth->path);
        }
    }

    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        line[strcspn(line, "\r\n")] = '\0';

        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') {
            continue;
        }

        char *colon = strchr(p, ':');
        if (!colon || colon == p) {
            log_warn("%s:%d: expected user:hash", auth->path, line_num);
            continue;
        }
        *colon = '\0';
        const char *user = p;
        const char *hash = colon + 1;

        /* Only modern crypt(3) schemes ("$id$..."); refuse plaintext and
         * legacy DES hashes */
        if (hash[0] != '$' || strlen(hash) >= AUTH_MAX_FIELD ||
            strlen(user) >= AUTH_MAX_FIELD) {
            log_warn("%s:%d: unsupported or invalid hash for user '%s' "
                     "(use htpasswd -B or openssl passwd -6)",
                     auth->path, line_num, user);
            continue;
        }

        if (count == capacity) {
            size_t new_cap = capacity ? capacity * 2 : 8;
            auth_entry_t *grown = realloc(entries, new_cap * sizeof(*entries));
            if (!grown) {
                free(entries);
                fclose(fp);
                return WS_ENOMEM;
            }
            entries = grown;
            capacity = new_cap;
        }
        strlcpy(entries[count].user, user, sizeof(entries[count].user));
        strlcpy(entries[count].hash, hash, sizeof(entries[count].hash));
        count++;
    }
    fclose(fp);

    free(auth->entries);
    auth->entries = entries;
    auth->count = count;
    auth->last_ok[0] = '\0';

    log_info("Loaded %zu user(s) from %s", count, auth->path);
    return WS_SUCCESS;
}

auth_t *auth_create(const char *path, const char *realm)
{
    if (!path || !auth_supported()) {
        log_error("This build of ws2socket has no password support (libcrypt missing)");
        return NULL;
    }

    auth_t *auth = calloc(1, sizeof(*auth));
    if (!auth) {
        return NULL;
    }
    strlcpy(auth->path, path, sizeof(auth->path));
    strlcpy(auth->realm, (realm && *realm) ? realm : "ws2socket", sizeof(auth->realm));

    if (auth_load(auth) != WS_SUCCESS) {
        auth_destroy(auth);
        return NULL;
    }
    if (auth->count == 0) {
        log_warn("Password file %s has no usable entries: every request will be refused",
                 path);
    }
    return auth;
}

void auth_destroy(auth_t *auth)
{
    if (!auth) {
        return;
    }
    free(auth->entries);
    OPENSSL_cleanse(auth->last_ok, sizeof(auth->last_ok));
    free(auth);
}

const char *auth_realm(const auth_t *auth)
{
    return auth ? auth->realm : "ws2socket";
}

/**
 * @brief Verify a password against a crypt(3) hash in constant time
 */
static int verify_password(const char *password, const char *hash)
{
#ifdef WS2SOCKET_HAVE_CRYPT
    /* struct crypt_data is large; one per process is enough */
    static struct crypt_data data;
    memset(&data, 0, sizeof(data));

    const char *computed = crypt_r(password, hash, &data);
    int ok = computed && computed[0] != '*' &&
             strlen(computed) == strlen(hash) &&
             CRYPTO_memcmp(computed, hash, strlen(hash)) == 0;
    OPENSSL_cleanse(&data, sizeof(data));
    return ok;
#else
    (void)password;
    (void)hash;
    return 0;
#endif
}

auth_result_t auth_check(auth_t *auth, const char *authorization,
                         char *user_out, size_t user_out_size)
{
    uint8_t decoded[2 * AUTH_MAX_FIELD + 2];
    struct stat st;

    if (!auth) {
        return AUTH_INVALID;
    }

    /* Pick up edits to the password file */
    if (stat(auth->path, &st) == 0 && st.st_mtime != auth->mtime) {
        auth_load(auth);
    }

    if (!authorization || strncasecmp(authorization, "Basic ", 6) != 0) {
        return AUTH_MISSING;
    }

    const char *encoded = authorization + 6;
    while (*encoded == ' ') encoded++;

    ssize_t len = base64_decode(encoded, decoded, sizeof(decoded) - 1);
    if (len <= 0) {
        return AUTH_INVALID;
    }
    decoded[len] = '\0';
    if (memchr(decoded, '\0', (size_t)len)) {
        OPENSSL_cleanse(decoded, sizeof(decoded));
        return AUTH_INVALID;
    }

    char *colon = strchr((char *)decoded, ':');
    if (!colon) {
        OPENSSL_cleanse(decoded, sizeof(decoded));
        return AUTH_INVALID;
    }
    *colon = '\0';
    const char *user = (const char *)decoded;
    const char *password = colon + 1;

    auth_result_t result = AUTH_INVALID;

    if (auth->last_ok[0] && strcmp(auth->last_ok, authorization) == 0) {
        result = AUTH_OK;  /* Same credentials as the previous request */
    } else {
        const auth_entry_t *entry = NULL;
        for (size_t i = 0; i < auth->count; i++) {
            if (strcmp(auth->entries[i].user, user) == 0) {
                entry = &auth->entries[i];
                break;
            }
        }

        /* Hash even for unknown users, so response time does not reveal
         * which user names exist */
        const char *hash = entry ? entry->hash :
                           (auth->count > 0 ? auth->entries[0].hash : "$6$x$");
        int ok = verify_password(password, hash);
        if (ok && entry) {
            result = AUTH_OK;
            strlcpy(auth->last_ok, authorization, sizeof(auth->last_ok));
        }
    }

    if (result == AUTH_OK && user_out && user_out_size > 0) {
        strlcpy(user_out, user, user_out_size);
    }

    OPENSSL_cleanse(decoded, sizeof(decoded));
    return result;
}
