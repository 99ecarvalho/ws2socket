/**
 * @file token_auth.c
 * @brief Token-based Authentication Implementation
 * @author WebSocket to TCP Proxy Project
 * @version 0.1.0
 * 
 * License: LGPL v3
 */

#include "token_auth.h"
#include "logging.h"
#include "utils.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

/* Simple hash table entry */
typedef struct token_entry_s {
    char *token;
    token_target_t target;
    struct token_entry_s *next;
} token_entry_t;

#define TOKEN_HASH_SIZE 1024

/* Internal hash table structure */
typedef struct {
    token_entry_t *buckets[TOKEN_HASH_SIZE];
    int count;
} token_hash_t;

/* Simple hash function */
static unsigned int hash_string(const char *str)
{
    unsigned int hash = 5381;
    int c;
    
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    
    return hash % TOKEN_HASH_SIZE;
}

/* Free hash table */
static void hash_free(token_hash_t *hash)
{
    if (!hash) return;
    
    for (int i = 0; i < TOKEN_HASH_SIZE; i++) {
        token_entry_t *entry = hash->buckets[i];
        while (entry) {
            token_entry_t *next = entry->next;
            free(entry->token);
            free(entry);
            entry = next;
        }
    }
    free(hash);
}

/* Insert token into hash table */
static int hash_insert(token_hash_t *hash, const char *token, const char *host, uint16_t port)
{
    unsigned int bucket = hash_string(token);
    
    /* Check if token already exists */
    token_entry_t *entry = hash->buckets[bucket];
    while (entry) {
        if (strcmp(entry->token, token) == 0) {
            /* Update existing */
            strlcpy(entry->target.host, host, sizeof(entry->target.host));
            entry->target.port = port;
            return WS_SUCCESS;
        }
        entry = entry->next;
    }
    
    /* Create new entry */
    entry = (token_entry_t *)calloc(1, sizeof(token_entry_t));
    if (!entry) return WS_ENOMEM;
    
    entry->token = strdup(token);
    if (!entry->token) {
        free(entry);
        return WS_ENOMEM;
    }
    
    strlcpy(entry->target.host, host, sizeof(entry->target.host));
    entry->target.port = port;
    
    /* Insert at head of bucket */
    entry->next = hash->buckets[bucket];
    hash->buckets[bucket] = entry;
    hash->count++;
    
    return WS_SUCCESS;
}

/* Lookup token in hash table */
static token_entry_t *hash_lookup(token_hash_t *hash, const char *token)
{
    unsigned int bucket = hash_string(token);
    token_entry_t *entry = hash->buckets[bucket];
    
    while (entry) {
        if (strcmp(entry->token, token) == 0) {
            return entry;
        }
        entry = entry->next;
    }
    
    return NULL;
}

/**
 * @brief Create token authentication manager
 */
token_auth_t *token_auth_create(const char *token_file, int auto_reload)
{
    token_auth_t *auth;
    
    if (!token_file) {
        return NULL;
    }
    
    auth = (token_auth_t *)calloc(1, sizeof(token_auth_t));
    if (!auth) {
        return NULL;
    }
    
    strlcpy(auth->token_file, token_file, sizeof(auth->token_file));
    auth->auto_reload = auto_reload;
    auth->last_load = 0;
    pthread_mutex_init(&auth->lock, NULL);
    
    /* Create hash table */
    auth->tokens = calloc(1, sizeof(token_hash_t));
    if (!auth->tokens) {
        free(auth);
        return NULL;
    }
    
    /* Load tokens */
    if (token_auth_load(auth) != WS_SUCCESS) {
        log_warn("Failed to load initial tokens from %s", token_file);
    }
    
    return auth;
}

/**
 * @brief Destroy token authentication manager
 */
void token_auth_destroy(token_auth_t *auth)
{
    if (!auth) return;
    
    pthread_mutex_lock(&auth->lock);
    
    if (auth->tokens) {
        hash_free((token_hash_t *)auth->tokens);
    }
    
    pthread_mutex_unlock(&auth->lock);
    pthread_mutex_destroy(&auth->lock);
    
    free(auth);
}

/* Load tokens from a single file */
static int load_token_file(token_hash_t *hash, const char *filename)
{
    FILE *fp;
    char line[1024];
    int line_num = 0;
    int loaded = 0;
    
    fp = fopen(filename, "r");
    if (!fp) {
        log_error("Failed to open token file '%s': %s", filename, strerror(errno));
        return WS_ERROR;
    }
    
    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        
        /* Remove newline */
        line[strcspn(line, "\r\n")] = '\0';
        
        /* Skip empty lines and comments */
        char *trimmed = line;
        while (*trimmed && isspace(*trimmed)) trimmed++;
        if (*trimmed == '\0' || *trimmed == '#') continue;
        
        /* Parse: token: host:port */
        char *colon = strchr(trimmed, ':');
        if (!colon) {
            log_warn("Syntax error in %s line %d: no colon separator", filename, line_num);
            continue;
        }
        
        *colon = '\0';
        char *token = trimmed;
        char *target = colon + 1;
        
        /* Trim token */
        char *end = token + strlen(token) - 1;
        while (end > token && isspace(*end)) *end-- = '\0';
        
        /* Trim target */
        while (*target && isspace(*target)) target++;
        
        if (strlen(token) == 0 || strlen(target) == 0) {
            log_warn("Empty token or target in %s line %d", filename, line_num);
            continue;
        }
        
        /* Parse target host:port */
        char host[256];
        uint16_t port;
        if (parse_hostport(target, host, sizeof(host), &port) != WS_SUCCESS) {
            log_warn("Invalid target '%s' in %s line %d", target, filename, line_num);
            continue;
        }
        
        /* Insert into hash table */
        if (hash_insert(hash, token, host, port) == WS_SUCCESS) {
            loaded++;
            log_debug("Loaded token '%s' -> %s:%u", token, host, port);
        }
    }
    
    fclose(fp);
    log_info("Loaded %d tokens from %s", loaded, filename);
    return WS_SUCCESS;
}

/**
 * @brief Load/reload tokens from file or directory
 */
int token_auth_load(token_auth_t *auth)
{
    struct stat st;
    token_hash_t *new_hash;
    token_hash_t *old_hash;
    
    if (!auth) return WS_EINVAL;
    
    pthread_mutex_lock(&auth->lock);
    
    /* Check if file/directory exists */
    if (stat(auth->token_file, &st) != 0) {
        log_error("Token file/directory not found: %s", auth->token_file);
        pthread_mutex_unlock(&auth->lock);
        return WS_ERROR;
    }
    
    /* Create new hash table */
    new_hash = (token_hash_t *)calloc(1, sizeof(token_hash_t));
    if (!new_hash) {
        pthread_mutex_unlock(&auth->lock);
        return WS_ENOMEM;
    }
    
    /* Load from file or directory */
    if (S_ISDIR(st.st_mode)) {
        /* Load from directory */
        DIR *dir = opendir(auth->token_file);
        if (!dir) {
            log_error("Failed to open directory %s: %s", auth->token_file, strerror(errno));
            free(new_hash);
            pthread_mutex_unlock(&auth->lock);
            return WS_ERROR;
        }
        
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN) {
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "%s/%s", auth->token_file, entry->d_name);
                load_token_file(new_hash, filepath);
            }
        }
        
        closedir(dir);
    } else {
        /* Load from single file */
        load_token_file(new_hash, auth->token_file);
    }
    
    /* Replace old hash table */
    old_hash = (token_hash_t *)auth->tokens;
    auth->tokens = new_hash;
    auth->last_load = time(NULL);
    
    if (old_hash) {
        hash_free(old_hash);
    }
    
    log_info("Token authentication loaded: %d tokens", new_hash->count);
    
    pthread_mutex_unlock(&auth->lock);
    return WS_SUCCESS;
}

/**
 * @brief Lookup target for token
 */
int token_auth_lookup(token_auth_t *auth, const char *token, token_target_t *target_out)
{
    token_entry_t *entry;
    
    if (!auth || !token || !target_out) {
        return WS_EINVAL;
    }
    
    pthread_mutex_lock(&auth->lock);
    
    /* Auto-reload if enabled (every 60 seconds) */
    if (auth->auto_reload && (time(NULL) - auth->last_load) > 60) {
        log_debug("Auto-reloading tokens");
        pthread_mutex_unlock(&auth->lock);
        token_auth_load(auth);
        pthread_mutex_lock(&auth->lock);
    }
    
    /* Lookup token */
    entry = hash_lookup((token_hash_t *)auth->tokens, token);
    if (!entry) {
        pthread_mutex_unlock(&auth->lock);
        log_debug("Token not found: %s", token);
        return WS_EAUTH;
    }
    
    /* Copy target */
    memcpy(target_out, &entry->target, sizeof(token_target_t));
    
    pthread_mutex_unlock(&auth->lock);
    
    log_info("Token authenticated: %s -> %s:%u", token, 
            target_out->host, target_out->port);
    
    return WS_SUCCESS;
}

/**
 * @brief Extract token from WebSocket path
 */
int token_auth_extract_from_path(const char *path, char *token_out, size_t token_out_size)
{
    const char *query;
    const char *token_start;
    
    if (!path || !token_out || token_out_size == 0) {
        return WS_EINVAL;
    }
    
    /* Format 1: /websockify?token=TOKEN */
    query = strchr(path, '?');
    if (query) {
        const char *token_param = strstr(query, "token=");
        if (token_param) {
            token_start = token_param + 6; /* Skip "token=" */
            const char *end = strchr(token_start, '&');
            size_t len = end ? (size_t)(end - token_start) : strlen(token_start);
            
            if (len > 0 && len < token_out_size) {
                memcpy(token_out, token_start, len);
                token_out[len] = '\0';
                return WS_SUCCESS;
            }
        }
    }
    
    /* Format 2: /TOKEN/ or /TOKEN */
    if (path[0] == '/') {
        token_start = path + 1;
        const char *slash = strchr(token_start, '/');
        const char *question = strchr(token_start, '?');
        
        /* Find end of token */
        const char *end = NULL;
        if (slash && question) {
            end = (slash < question) ? slash : question;
        } else if (slash) {
            end = slash;
        } else if (question) {
            end = question;
        }
        
        size_t len = end ? (size_t)(end - token_start) : strlen(token_start);
        
        if (len > 0 && len < token_out_size && 
            strcmp(token_start, "websockify") != 0) {
            memcpy(token_out, token_start, len);
            token_out[len] = '\0';
            return WS_SUCCESS;
        }
    }
    
    return WS_ERROR;
}
