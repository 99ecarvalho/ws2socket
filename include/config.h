/**
 * @file config.h
 * @brief Configuration and command-line argument parsing
 * @author WebSocket to TCP Proxy Project
 * @version 0.1.0
 * 
 * Handles configuration file parsing and command-line argument processing
 * for ws2socket.
 * 
 * License: LGPL v3
 */

#ifndef WS2SOCKET_CONFIG_H
#define WS2SOCKET_CONFIG_H

#include "common.h"
#include "server.h"
#include "proxy.h"

/**
 * @struct app_config
 * @brief Complete application configuration
 * 
 * Combines server and proxy configuration with additional settings.
 */
typedef struct {
    /** Server configuration */
    server_config_t server;
    /** Proxy configuration */
    proxy_config_t proxy;
    /** Log configuration */
    struct {
        /** Log level */
        int level;
        /** Log targets (console, syslog, file) */
        int targets;
        /** Log file path */
        char logfile[512];
        /** Syslog facility */
        int syslog_facility;
    } logging;
    /** Token-based authentication enabled */
    int token_auth;
    /** Token file path */
    char token_file[512];
    /** Default target host:port */
    char target_host[256];
    uint16_t target_port;
    /** Allow connecting to any target */
    int allow_any_target;
    /** Daemonize process */
    int daemonize;
    /** PID file path */
    char pid_file[512];
    /** Configuration file path */
    char config_file[512];
    /** Web root directory for static files (e.g., noVNC) */
    char web_root[512];
} app_config_t;

/**
 * @brief Initialize configuration with defaults
 * 
 * Sets up app_config_t with reasonable default values.
 * 
 * @param config Pointer to config structure
 * @return WS_SUCCESS on success, error code otherwise
 */
int config_init_defaults(app_config_t *config);

/**
 * @brief Parse command-line arguments
 * 
 * Parses command-line arguments and updates configuration.
 * 
 * @param argc Argument count
 * @param argv Argument array
 * @param config Pointer to config structure to update
 * @return WS_SUCCESS on success, error code on error
 * 
 * @note Call config_init_defaults() first to set defaults
 */
int config_parse_args(int argc, char *argv[], app_config_t *config);

/**
 * @brief Load configuration from file
 * 
 * Reads configuration file and updates configuration structure.
 * Supports INI-style format.
 * 
 * @param filename Path to configuration file
 * @param config Pointer to config structure to update
 * @return WS_SUCCESS on success, error code otherwise
 */
int config_load_file(const char *filename, app_config_t *config);

/**
 * @brief Validate configuration
 * 
 * Checks that all required configuration options are set
 * and values are valid.
 * 
 * @param config Pointer to configuration structure
 * @return WS_SUCCESS if valid, error code otherwise
 */
int config_validate(const app_config_t *config);

/**
 * @brief Print configuration
 * 
 * Outputs current configuration for debugging.
 * 
 * @param config Pointer to configuration structure
 */
void config_print(const app_config_t *config);

/**
 * @brief Print usage information
 * 
 * Prints command-line usage and available options.
 * 
 * @param program_name Name of the program
 */
void config_print_usage(const char *program_name);

/**
 * @brief Print version information
 */
void config_print_version(void);

#endif /* WS2SOCKET_CONFIG_H */
