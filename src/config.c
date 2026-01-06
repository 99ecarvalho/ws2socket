/**
 * @file config.c
 * @brief Configuration Parsing
 * @author Eduardo Correia <ecorreia@apliant.com.br>
 * 
 * License: LGPL v3
 */

#include "config.h"
#include "logging.h"
#include "utils.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

/**
 * @brief Initialize configuration with defaults
 */
int config_init_defaults(app_config_t *config)
{
    if (!config) {
        return WS_EINVAL;
    }

    memset(config, 0, sizeof(app_config_t));

    /* Server defaults */
    strlcpy(config->server.listen_host, "0.0.0.0", sizeof(config->server.listen_host));
    config->server.listen_port = DEFAULT_WS_PORT;
    config->server.use_ssl = 0;
    config->server.only_upgrade = 1;
    config->server.verbose = 0;
    config->server.traffic = 0;
    config->server.max_connections = MAX_CONNECTIONS;
    config->server.socket_timeout = SOCKET_TIMEOUT;
    config->server.http_timeout = 30;
    config->server.keepalive = 1;
    config->server.backlog = 32;

    /* Proxy defaults */
    config->proxy.max_connections = MAX_CONNECTIONS;
    config->proxy.buffer_size = MAX_FRAME_SIZE;
    config->proxy.socket_timeout = SOCKET_TIMEOUT;
    config->proxy.tcp_nodelay = 1;
    config->proxy.log_traffic = 0;
    config->proxy.heartbeat_enabled = 0;
    config->proxy.heartbeat_interval = 60000;

    /* Logging defaults */
    config->logging.level = LOG_INFO;
    config->logging.targets = LOG_TARGET_CONSOLE;
    config->logging.syslog_facility = 16;  /* LOG_LOCAL0 */

    /* Application defaults */
    config->token_auth = 0;
    config->allow_any_target = 0;
    config->daemonize = 0;

    return WS_SUCCESS;
}

/**
 * @brief Parse command-line arguments
 */
int config_parse_args(int argc, char *argv[], app_config_t *config)
{
    int i;

    if (!argv || !config) {
        return WS_EINVAL;
    }

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            config_print_usage(argv[0]);
            exit(0);
        } else if (strcmp(arg, "--version") == 0) {
            config_print_version();
            exit(0);
        } else if (strcmp(arg, "-l") == 0 || strcmp(arg, "--listen") == 0) {
            if (++i >= argc) {
                log_error("Missing argument for %s", arg);
                return WS_EINVAL;
            }
            char host[256];
            uint16_t port;
            if (parse_hostport(argv[i], host, sizeof(host), &port) == WS_SUCCESS) {
                strlcpy(config->server.listen_host, host, sizeof(config->server.listen_host));
                if (port > 0) {
                    config->server.listen_port = port;
                } else {
                    config->server.listen_port = DEFAULT_WS_PORT;
                }
            }
        } else if (strcmp(arg, "-p") == 0 || strcmp(arg, "--port") == 0) {
            if (++i >= argc) {
                log_error("Missing argument for %s", arg);
                return WS_EINVAL;
            }
            config->server.listen_port = (uint16_t)atoi(argv[i]);
        } else if (strcmp(arg, "-t") == 0 || strcmp(arg, "--target") == 0) {
            if (++i >= argc) {
                log_error("Missing argument for %s", arg);
                return WS_EINVAL;
            }
            char host[256];
            uint16_t port;
            if (parse_hostport(argv[i], host, sizeof(host), &port) == WS_SUCCESS) {
                strlcpy(config->target_host, host, sizeof(config->target_host));
                config->target_port = port;
            }
        } else if (strcmp(arg, "-c") == 0 || strcmp(arg, "--cert") == 0) {
            if (++i >= argc) {
                log_error("Missing argument for %s", arg);
                return WS_EINVAL;
            }
            strlcpy(config->server.cert_file, argv[i], sizeof(config->server.cert_file));
            config->server.use_ssl = 1;
        } else if (strcmp(arg, "-k") == 0 || strcmp(arg, "--key") == 0) {
            if (++i >= argc) {
                log_error("Missing argument for %s", arg);
                return WS_EINVAL;
            }
            strlcpy(config->server.key_file, argv[i], sizeof(config->server.key_file));
            config->server.use_ssl = 1;
        } else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            config->server.verbose = 1;
            config->logging.level = LOG_DEBUG;
        } else if (strcmp(arg, "--log-file") == 0) {
            if (++i >= argc) {
                log_error("Missing argument for %s", arg);
                return WS_EINVAL;
            }
            strlcpy(config->logging.logfile, argv[i], sizeof(config->logging.logfile));
            config->logging.targets |= LOG_TARGET_FILE;
        } else if (strcmp(arg, "--daemon") == 0) {
            config->daemonize = 1;
        } else if (strcmp(arg, "--pid-file") == 0) {
            if (++i >= argc) {
                log_error("Missing argument for %s", arg);
                return WS_EINVAL;
            }
            strlcpy(config->pid_file, argv[i], sizeof(config->pid_file));
        } else if (strcmp(arg, "-w") == 0 || strcmp(arg, "--web-root") == 0) {
            if (++i >= argc) {
                log_error("Missing argument for %s", arg);
                return WS_EINVAL;
            }
            strlcpy(config->web_root, argv[i], sizeof(config->web_root));
        } else if (strcmp(arg, "-f") == 0 || strcmp(arg, "--config") == 0) {
            if (++i >= argc) {
                log_error("Missing argument for %s", arg);
                return WS_EINVAL;
            }
            strlcpy(config->config_file, argv[i], sizeof(config->config_file));
        } else {
            log_error("Unknown option: %s", arg);
            return WS_EINVAL;
        }
    }

    return WS_SUCCESS;
}

/**
 * @brief Load configuration from file
 */
int config_load_file(const char *filename, app_config_t *config)
{
    FILE *fp;
    char line[1024];
    char section[64] = "";
    char *key, *value, *comment;
    int line_num = 0;

    if (!filename || !config) {
        return WS_EINVAL;
    }

    fp = fopen(filename, "r");
    if (!fp) {
        log_error("Failed to open config file '%s': %s", filename, strerror(errno));
        return WS_ERROR;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        line_num++;

        /* Remove trailing newline */
        line[strcspn(line, "\r\n")] = '\0';

        /* Remove comments */
        comment = strchr(line, '#');
        if (comment) {
            *comment = '\0';
        }
        comment = strchr(line, ';');
        if (comment) {
            *comment = '\0';
        }

        /* Trim leading whitespace */
        char *trimmed = line;
        while (*trimmed && isspace(*trimmed)) {
            trimmed++;
        }

        /* Skip empty lines */
        if (*trimmed == '\0') {
            continue;
        }

        /* Trim trailing whitespace */
        char *end = trimmed + strlen(trimmed) - 1;
        while (end > trimmed && isspace(*end)) {
            *end = '\0';
            end--;
        }

        /* Check for section header [section] */
        if (trimmed[0] == '[') {
            char *section_end = strchr(trimmed, ']');
            if (section_end) {
                *section_end = '\0';
                strlcpy(section, trimmed + 1, sizeof(section));
                continue;
            } else {
                log_warn("Invalid section at line %d: %s", line_num, trimmed);
                continue;
            }
        }

        /* Parse key=value */
        char *equals = strchr(trimmed, '=');
        if (!equals) {
            log_warn("Invalid line %d (no '=' found): %s", line_num, trimmed);
            continue;
        }

        *equals = '\0';
        key = trimmed;
        value = equals + 1;

        /* Trim key */
        end = key + strlen(key) - 1;
        while (end > key && isspace(*end)) {
            *end = '\0';
            end--;
        }

        /* Trim value */
        while (*value && isspace(*value)) {
            value++;
        }

        /* Parse configuration based on section */
        if (strcmp(section, "server") == 0) {
            if (strcmp(key, "listen") == 0) {
                char host[256];
                uint16_t port;
                if (parse_hostport(value, host, sizeof(host), &port) == WS_SUCCESS) {
                    strlcpy(config->server.listen_host, host, sizeof(config->server.listen_host));
                    if (port > 0) config->server.listen_port = port;
                }
            } else if (strcmp(key, "port") == 0) {
                config->server.listen_port = (uint16_t)atoi(value);
            } else if (strcmp(key, "cert_file") == 0) {
                strlcpy(config->server.cert_file, value, sizeof(config->server.cert_file));
                config->server.use_ssl = 1;
            } else if (strcmp(key, "key_file") == 0) {
                strlcpy(config->server.key_file, value, sizeof(config->server.key_file));
                config->server.use_ssl = 1;
            } else if (strcmp(key, "max_connections") == 0) {
                config->server.max_connections = atoi(value);
            } else if (strcmp(key, "socket_timeout") == 0) {
                config->server.socket_timeout = atoi(value);
            } else if (strcmp(key, "web_root") == 0) {
                strlcpy(config->web_root, value, sizeof(config->web_root));
            }
        } else if (strcmp(section, "proxy") == 0) {
            if (strcmp(key, "target") == 0) {
                char host[256];
                uint16_t port;
                if (parse_hostport(value, host, sizeof(host), &port) == WS_SUCCESS) {
                    strlcpy(config->target_host, host, sizeof(config->target_host));
                    config->target_port = port;
                }
            } else if (strcmp(key, "buffer_size") == 0) {
                config->proxy.buffer_size = (size_t)atoi(value);
            } else if (strcmp(key, "max_connections") == 0) {
                config->proxy.max_connections = atoi(value);
            } else if (strcmp(key, "socket_timeout") == 0) {
                config->proxy.socket_timeout = atoi(value);
            }
        } else if (strcmp(section, "logging") == 0) {
            if (strcmp(key, "level") == 0) {
                if (strcmp(value, "debug") == 0) config->logging.level = LOG_DEBUG;
                else if (strcmp(value, "info") == 0) config->logging.level = LOG_INFO;
                else if (strcmp(value, "warning") == 0) config->logging.level = LOG_WARN;
                else if (strcmp(value, "error") == 0) config->logging.level = LOG_ERROR;
                else if (strcmp(value, "critical") == 0) config->logging.level = LOG_CRITICAL;
            } else if (strcmp(key, "file") == 0) {
                strlcpy(config->logging.logfile, value, sizeof(config->logging.logfile));
                config->logging.targets |= LOG_TARGET_FILE;
            } else if (strcmp(key, "console") == 0) {
                if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
                    config->logging.targets |= LOG_TARGET_CONSOLE;
                }
            } else if (strcmp(key, "syslog") == 0) {
                if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
                    config->logging.targets |= LOG_TARGET_SYSLOG;
                }
            }
        } else if (strcmp(section, "general") == 0) {
            if (strcmp(key, "daemon") == 0) {
                config->daemonize = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            } else if (strcmp(key, "pid_file") == 0) {
                strlcpy(config->pid_file, value, sizeof(config->pid_file));
            } else if (strcmp(key, "token_file") == 0) {
                strlcpy(config->token_file, value, sizeof(config->token_file));
                config->token_auth = 1;
            }
        }
    }

    fclose(fp);
    log_info("Loaded configuration from '%s'", filename);
    return WS_SUCCESS;
}

/**
 * @brief Validate configuration
 */
int config_validate(const app_config_t *config)
{
    if (!config) {
        return WS_EINVAL;
    }

    if (config->server.listen_port == 0) {
        log_error("Invalid listen port");
        return WS_EINVAL;
    }

    if (config->server.use_ssl) {
        if (strlen(config->server.cert_file) == 0) {
            log_error("SSL enabled but no certificate file specified");
            return WS_EINVAL;
        }
        if (strlen(config->server.key_file) == 0) {
            log_error("SSL enabled but no key file specified");
            return WS_EINVAL;
        }
    }

    if (!config->allow_any_target && strlen(config->target_host) == 0) {
        log_error("No default target specified and allow_any_target disabled");
        return WS_EINVAL;
    }

    return WS_SUCCESS;
}

/**
 * @brief Print configuration
 */
void config_print(const app_config_t *config)
{
    if (!config) {
        return;
    }

    log_info("=== Configuration ===");
    log_info("Listen: %s:%u", config->server.listen_host, config->server.listen_port);
    log_info("SSL: %s", config->server.use_ssl ? "enabled" : "disabled");
    log_info("Target: %s:%u", config->target_host, config->target_port);
    log_info("Max Connections: %d", config->server.max_connections);
    log_info("Verbose: %s", config->server.verbose ? "yes" : "no");
    log_info("======================");
}

/**
 * @brief Print usage information
 */
void config_print_usage(const char *program_name)
{
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("\nOptions:\n");
    printf("  -h, --help                Print this help message\n");
    printf("  --version                 Print version information\n");
    printf("  -l, --listen HOST[:PORT]  Listen address (default: 0.0.0.0:6080)\n");
    printf("  -p, --port PORT           Listen port (default: 6080)\n");
    printf("  -t, --target HOST:PORT    Default target server\n");
    printf("  -c, --cert FILE           SSL certificate file\n");
    printf("  -k, --key FILE            SSL private key file\n");
    printf("  -v, --verbose             Verbose output\n");
    printf("  -w, --web-root DIR        Web root directory for static files (noVNC)\n");
    printf("  -f, --config FILE         Configuration file path\n");
    printf("  --log-file FILE           Log file path\n");
    printf("  --daemon                  Daemonize process\n");
    printf("  --pid-file FILE           PID file path\n");
    printf("\nExamples:\n");
    printf("  %s --listen 127.0.0.1:6080 --target 192.168.1.1:5900\n", program_name);
    printf("  %s --cert cert.pem --key key.pem --target example.com:22\n", program_name);
}

/**
 * @brief Print version information
 */
void config_print_version(void)
{
    printf("ws2socket version 0.1.0\n");
    printf("WebSocket to TCP Socket Proxy\n");
    printf("License: LGPL v3\n");
}
