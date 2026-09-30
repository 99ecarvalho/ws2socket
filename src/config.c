/**
 * @file config.c
 * @brief Configuration Parsing
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

#include "config.h"
#include "logging.h"
#include "utils.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>

/**
 * @brief Parse a TCP port number (1-65535)
 */
static int parse_port(const char *value, uint16_t *port_out)
{
    char *end;
    long port;

    errno = 0;
    port = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || port < 1 || port > 65535) {
        return WS_EINVAL;
    }

    *port_out = (uint16_t)port;
    return WS_SUCCESS;
}

/**
 * @brief Parse a boolean value (true/false, yes/no, on/off, 1/0)
 *
 * @return 1 or 0, or -1 if the value is not a boolean
 */
static int parse_bool(const char *value)
{
    if (strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0 ||
        strcasecmp(value, "on") == 0 || strcmp(value, "1") == 0) {
        return 1;
    }
    if (strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0 ||
        strcasecmp(value, "off") == 0 || strcmp(value, "0") == 0) {
        return 0;
    }
    return -1;
}

/**
 * @brief Parse a listen address: HOST:PORT, HOST, or just PORT
 */
static int parse_listen(const char *value, server_config_t *server)
{
    char host[256];
    uint16_t port;

    /* A bare number is a port, as in websockify */
    if (parse_port(value, &port) == WS_SUCCESS) {
        server->listen_port = port;
        return WS_SUCCESS;
    }

    if (parse_hostport(value, host, sizeof(host), &port) != WS_SUCCESS ||
        host[0] == '\0') {
        return WS_EINVAL;
    }

    strlcpy(server->listen_host, host, sizeof(server->listen_host));
    if (port > 0) {
        server->listen_port = port;
    }
    return WS_SUCCESS;
}

/**
 * @brief Parse a target address, which must be HOST:PORT
 */
static int parse_target(const char *value, app_config_t *config)
{
    char host[256];
    uint16_t port;

    if (parse_hostport(value, host, sizeof(host), &port) != WS_SUCCESS ||
        host[0] == '\0' || port == 0) {
        return WS_EINVAL;
    }

    strlcpy(config->target_host, host, sizeof(config->target_host));
    config->target_port = port;
    return WS_SUCCESS;
}

/**
 * @brief Set or clear a logging target bit from a boolean value
 */
static int set_log_target(app_config_t *config, int target, const char *value)
{
    int enabled = parse_bool(value);
    if (enabled < 0) {
        return WS_EINVAL;
    }
    if (enabled) {
        config->logging.targets |= target;
    } else {
        config->logging.targets &= ~target;
    }
    return WS_SUCCESS;
}

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
    config->proxy.buffer_size = DEFAULT_MAX_MESSAGE_SIZE;
    config->proxy.socket_timeout = SOCKET_TIMEOUT;
    config->proxy.tcp_nodelay = 1;
    config->proxy.log_traffic = 0;
    config->proxy.heartbeat_enabled = 0;
    config->proxy.heartbeat_interval = 60000;

    /* Logging defaults */
    config->logging.level = WS_LOG_INFO;
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
            if (parse_listen(argv[i], &config->server) != WS_SUCCESS) {
                log_error("Invalid listen address '%s' (expected HOST[:PORT] or PORT)", argv[i]);
                return WS_EINVAL;
            }
        } else if (strcmp(arg, "-p") == 0 || strcmp(arg, "--port") == 0) {
            if (++i >= argc) {
                log_error("Missing argument for %s", arg);
                return WS_EINVAL;
            }
            if (parse_port(argv[i], &config->server.listen_port) != WS_SUCCESS) {
                log_error("Invalid port '%s' (expected 1-65535)", argv[i]);
                return WS_EINVAL;
            }
        } else if (strcmp(arg, "-t") == 0 || strcmp(arg, "--target") == 0) {
            if (++i >= argc) {
                log_error("Missing argument for %s", arg);
                return WS_EINVAL;
            }
            if (parse_target(argv[i], config) != WS_SUCCESS) {
                log_error("Invalid target '%s' (expected HOST:PORT)", argv[i]);
                return WS_EINVAL;
            }
        } else if (strcmp(arg, "--token-file") == 0) {
            if (++i >= argc) {
                log_error("Missing argument for %s", arg);
                return WS_EINVAL;
            }
            strlcpy(config->token_file, argv[i], sizeof(config->token_file));
            config->token_auth = 1;
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
            config->logging.level = WS_LOG_DEBUG;
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
        int bad_value = 0;
        if (strcmp(section, "server") == 0) {
            if (strcmp(key, "listen") == 0) {
                bad_value = parse_listen(value, &config->server) != WS_SUCCESS;
            } else if (strcmp(key, "port") == 0) {
                bad_value = parse_port(value, &config->server.listen_port) != WS_SUCCESS;
            } else if (strcmp(key, "cert_file") == 0) {
                strlcpy(config->server.cert_file, value, sizeof(config->server.cert_file));
                config->server.use_ssl = 1;
            } else if (strcmp(key, "key_file") == 0) {
                strlcpy(config->server.key_file, value, sizeof(config->server.key_file));
                config->server.use_ssl = 1;
            } else if (strcmp(key, "max_connections") == 0) {
                config->server.max_connections = atoi(value);
                bad_value = config->server.max_connections < 0;
            } else if (strcmp(key, "socket_timeout") == 0) {
                config->server.socket_timeout = atoi(value);
                bad_value = config->server.socket_timeout < 0;
            } else if (strcmp(key, "web_root") == 0) {
                strlcpy(config->web_root, value, sizeof(config->web_root));
            }
        } else if (strcmp(section, "proxy") == 0) {
            if (strcmp(key, "target") == 0) {
                bad_value = parse_target(value, config) != WS_SUCCESS;
            } else if (strcmp(key, "buffer_size") == 0) {
                int size = atoi(value);
                bad_value = size <= 0;
                if (size > 0) config->proxy.buffer_size = (size_t)size;
            } else if (strcmp(key, "max_connections") == 0) {
                config->proxy.max_connections = atoi(value);
            } else if (strcmp(key, "socket_timeout") == 0) {
                config->proxy.socket_timeout = atoi(value);
                bad_value = config->proxy.socket_timeout < 0;
            }
        } else if (strcmp(section, "logging") == 0) {
            if (strcmp(key, "level") == 0) {
                if (strcmp(value, "debug") == 0) config->logging.level = WS_LOG_DEBUG;
                else if (strcmp(value, "info") == 0) config->logging.level = WS_LOG_INFO;
                else if (strcmp(value, "warning") == 0) config->logging.level = WS_LOG_WARN;
                else if (strcmp(value, "error") == 0) config->logging.level = WS_LOG_ERROR;
                else if (strcmp(value, "critical") == 0) config->logging.level = WS_LOG_CRITICAL;
                else bad_value = 1;
            } else if (strcmp(key, "file") == 0) {
                strlcpy(config->logging.logfile, value, sizeof(config->logging.logfile));
                config->logging.targets |= LOG_TARGET_FILE;
            } else if (strcmp(key, "console") == 0) {
                bad_value = set_log_target(config, LOG_TARGET_CONSOLE, value) != WS_SUCCESS;
            } else if (strcmp(key, "syslog") == 0) {
                bad_value = set_log_target(config, LOG_TARGET_SYSLOG, value) != WS_SUCCESS;
            }
        } else if (strcmp(section, "general") == 0) {
            if (strcmp(key, "daemon") == 0) {
                int daemon = parse_bool(value);
                bad_value = daemon < 0;
                if (daemon >= 0) config->daemonize = daemon;
            } else if (strcmp(key, "pid_file") == 0) {
                strlcpy(config->pid_file, value, sizeof(config->pid_file));
            } else if (strcmp(key, "token_file") == 0) {
                strlcpy(config->token_file, value, sizeof(config->token_file));
                config->token_auth = 1;
            }
        }

        if (bad_value) {
            log_error("%s:%d: invalid value for '%s': %s", filename, line_num, key, value);
            fclose(fp);
            return WS_EINVAL;
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

    if (!config->token_auth && strlen(config->target_host) == 0) {
        log_error("No target specified: use --target HOST:PORT or --token-file FILE");
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
    if (config->token_auth) {
        log_info("Target: from token file %s", config->token_file);
    } else {
        log_info("Target: %s:%u", config->target_host, config->target_port);
    }
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
    printf("  -l, --listen HOST[:PORT]  Listen address or port (default: 0.0.0.0:6080)\n");
    printf("  -p, --port PORT           Listen port (default: 6080)\n");
    printf("  -t, --target HOST:PORT    Target TCP server\n");
    printf("  --token-file FILE         Choose the target per client from a token file\n");
    printf("  -c, --cert FILE           TLS certificate file (PEM), enables wss://\n");
    printf("  -k, --key FILE            TLS private key file (PEM)\n");
    printf("  -v, --verbose             Verbose output\n");
    printf("  -w, --web-root DIR        Web root directory for static files (noVNC)\n");
    printf("  -f, --config FILE         Configuration file (command-line options override it)\n");
    printf("  --log-file FILE           Log file path\n");
    printf("  --daemon                  Daemonize process\n");
    printf("  --pid-file FILE           PID file path\n");
    printf("\nExamples:\n");
    printf("  %s --listen 127.0.0.1:6080 --target 192.168.1.1:5900\n", program_name);
    printf("  %s --cert cert.pem --key key.pem --target example.com:22\n", program_name);
    printf("\nReport bugs: https://github.com/99ecarvalho/ws2socket/issues\n");
}

/**
 * @brief Print version information
 */
void config_print_version(void)
{
    printf("ws2socket version 0.1.0\n");
    printf("WebSocket to TCP Socket Proxy\n");
    printf("Copyright (c) 2026 Eduardo Correia <ecorreia@apliant.com.br>\n");
    printf("License: LGPL-3.0-or-later <https://www.gnu.org/licenses/lgpl-3.0.html>\n");
    printf("This is free software: you are free to change and redistribute it.\n");
    printf("There is NO WARRANTY, to the extent permitted by law.\n");
}
