/**
 * @file ws2socket.c
 * @brief Main WebSocket to TCP Socket Proxy Application
 * @author Eduardo Correia <ecorreia@apliant.com.br>
 * 
 * Entry point for the ws2socket application.
 * Handles server initialization, client connections, and shutdown.
 * 
 * @copyright Copyright (c) 2026 Eduardo Correia <ecorreia@apliant.com.br>
 *
 * This file is part of ws2socket. It is free software, licensed under the
 * GNU Lesser General Public License v3.0 or later. See COPYING.LESSER
 * and COPYING for details.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "common.h"
#include "config.h"
#include "logging.h"
#include "server.h"
#include "proxy.h"
#include "websocket.h"
#include "utils.h"
#include "token_auth.h"
#include "auth.h"
#include "metrics.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

/** Global server instance */
static ws_server_t *g_server = NULL;

/** Global application configuration */
static app_config_t g_config;

/** Token-to-target map (NULL unless a token file is configured) */
static token_auth_t *g_token_auth = NULL;

/** Password database (NULL unless authentication is enabled) */
static auth_t *g_auth = NULL;

/**
 * @brief Signal handler for graceful shutdown
 * 
 * @param sig Signal number
 */
static void signal_handler(int sig)
{
    log_info("Received signal %d, shutting down...", sig);

    if (g_server) {
        server_stop(g_server);
    }

    exit(0);
}

/**
 * @brief Handle a WebSocket upgrade request: pick the target, connect,
 *        complete the handshake and proxy until either side closes
 *
 * @param client_fd Client socket
 * @param ssl TLS session (if any)
 * @param req The upgrade request
 * @param addr Client address
 * @param client_id Identifier used in log messages
 * @return Status code
 */
static int handle_websocket(int client_fd, SSL *ssl, http_request_t *req,
                            const struct sockaddr_storage *addr, uint32_t client_id)
{
    websocket_t *ws = NULL;
    proxy_client_t *proxy_client = NULL;
    int ret;

    /* Choose the target: from the token file, or the configured default */
    char target_host[256];
    uint16_t target_port;

    if (g_token_auth) {
        char token[256];
        token_target_t token_target;

        if (token_auth_extract_from_path(req->path, token, sizeof(token)) != WS_SUCCESS ||
            token_auth_lookup(g_token_auth, token, &token_target) != WS_SUCCESS) {
            log_warn("[Client %u] Rejected: missing or unknown token", client_id);
            metrics_rejected(METRIC_REJECT_TOKEN);
            http_send_status(client_fd, ssl, 403, "Forbidden");
            return WS_EAUTH;
        }
        strlcpy(target_host, token_target.host, sizeof(target_host));
        target_port = token_target.port;
    } else {
        strlcpy(target_host, g_config.target_host, sizeof(target_host));
        target_port = g_config.target_port;
    }

    /* Create WebSocket */
    ws = websocket_create();
    if (!ws) {
        log_error("Failed to create WebSocket");
        return WS_ENOMEM;
    }

    /* Initialize WebSocket */
    ret = websocket_init(ws, client_fd, ssl);
    if (ret != WS_SUCCESS) {
        log_error("Failed to initialize WebSocket: %d", ret);
        websocket_destroy(ws);
        return ret;
    }

    /* Build headers array for websocket_accept */
    const char *headers[MAX_HTTP_HEADERS];
    static char header_lines[MAX_HTTP_HEADERS][640];
    for (int i = 0; i < req->num_headers && i < MAX_HTTP_HEADERS; i++) {
        /* Safely format header line with explicit null termination */
        int written = snprintf(header_lines[i], sizeof(header_lines[i]), "%s: %s",
                              req->headers[i].name, req->headers[i].value);
        if (written >= (int)sizeof(header_lines[i])) {
            /* Truncation occurred - ensure null termination */
            header_lines[i][sizeof(header_lines[i]) - 1] = '\0';
        }
        headers[i] = header_lines[i];
    }

    /* Connect to the target before completing the handshake, so a client
     * whose target is unreachable gets an HTTP error instead of a WebSocket
     * that closes immediately */
    proxy_client = proxy_client_create();
    if (!proxy_client) {
        log_error("[Client %u] Failed to create proxy client", client_id);
        websocket_destroy(ws);
        return WS_ENOMEM;
    }
    proxy_client->client_id = client_id;

    ret = proxy_connect_target(proxy_client, target_host, target_port, 0);
    if (ret != WS_SUCCESS) {
        log_error("[Client %u] Failed to connect to target %s:%u", client_id, target_host, target_port);
        metrics_rejected(METRIC_REJECT_TARGET);
        http_send_status(client_fd, ssl, 502, "Bad Gateway");
        websocket_destroy(ws);
        proxy_client_destroy(proxy_client);
        return ret;
    }

    ret = websocket_accept(ws, headers, req->num_headers);
    if (ret != WS_SUCCESS) {
        log_error("[Client %u] Failed to accept WebSocket: %d", client_id, ret);
        http_send_status(client_fd, ssl, 400, "Bad Request");
        websocket_destroy(ws);
        proxy_client_destroy(proxy_client);
        return ret;
    }

    /* proxy_client_create() made its own WebSocket object; use ours */
    websocket_destroy(proxy_client->ws);
    proxy_client->ws = ws;
    proxy_client->client_id = client_id;
    memcpy(&proxy_client->src_addr, addr, sizeof(struct sockaddr_storage));
    proxy_client->src_addr_len = sizeof(struct sockaddr_storage);

    log_info("[Client %u] WebSocket connection established, proxying to %s:%u", 
             client_id, target_host, target_port);

    metrics_http_response(101);
    metrics_ws_session_started();
    time_t session_start = time(NULL);

    /* Start bidirectional proxy */
    proxy_forward(proxy_client);

    metrics_ws_session_ended((uint64_t)(time(NULL) - session_start));

    /* Cleanup */
    proxy_client_destroy(proxy_client);

    return WS_SUCCESS;
}

/**
 * @brief Close gracefully after an error response
 *
 * If the client is still sending (for example an oversized request),
 * closing at once makes the kernel send a TCP reset, which can destroy the
 * response before the client reads it. Stop writing and drain what the
 * client sends for a moment first.
 */
static void lingering_close(int client_fd, SSL *ssl)
{
    uint8_t discard[4096];
    size_t drained = 0;

    socket_set_timeout(client_fd, 1);
    if (!ssl) {
        shutdown(client_fd, SHUT_WR);
    }
    while (drained < 256 * 1024) {
        ssize_t n = io_recv(client_fd, ssl, discard, sizeof(discard));
        if (n <= 0) {
            break;
        }
        drained += (size_t)n;
    }
}

/**
 * @brief Check the request's credentials when authentication is enabled
 *
 * Sends 401 when they are missing or wrong.
 *
 * @return 1 if the request may proceed, 0 if it was answered with 401
 */
static int check_auth(int client_fd, SSL *ssl, const http_request_t *req,
                      uint32_t client_id)
{
    char user[256];
    char challenge[256];

    if (!g_auth) {
        return 1;
    }

    auth_result_t result = auth_check(g_auth, http_get_header_value(req, "Authorization"),
                                      user, sizeof(user));
    if (result == AUTH_OK) {
        metrics_auth(1);
        log_debug("[Client %u] Authenticated as '%s'", client_id, user);
        return 1;
    }

    metrics_rejected(METRIC_REJECT_AUTH);
    if (result == AUTH_INVALID) {
        metrics_auth(0);
        log_warn("[Client %u] Authentication failed", client_id);
        sleep(1);  /* Slow down password guessing */
    }

    snprintf(challenge, sizeof(challenge),
             "WWW-Authenticate: Basic realm=\"%s\", charset=\"UTF-8\"\r\n",
             auth_realm(g_auth));
    http_respond(client_fd, ssl, req, 401, challenge, "text/plain; charset=utf-8",
                 "Unauthorized\n", 13);
    return 0;
}

/**
 * @brief Answer GET /metrics with the Prometheus text format
 */
static void serve_metrics(int client_fd, SSL *ssl, const http_request_t *req)
{
    static char body[16384];

    if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "HEAD") != 0) {
        http_respond(client_fd, ssl, req, 405, "Allow: GET, HEAD\r\n",
                     "text/plain; charset=utf-8", "Method Not Allowed\n", 19);
        return;
    }

    ssize_t len = metrics_export_prometheus(body, sizeof(body));
    if (len < 0) {
        http_respond(client_fd, ssl, req, 500, NULL, "text/plain; charset=utf-8",
                     "Internal Server Error\n", 22);
        return;
    }
    http_respond(client_fd, ssl, req, 200, "Cache-Control: no-store\r\n",
                 "text/plain; version=0.0.4; charset=utf-8", body, (size_t)len);
}

/**
 * @brief Handle a client connection
 *
 * Serves HTTP requests (static files, /metrics) until the client closes a
 * keep-alive connection, or hands the connection over to the WebSocket proxy
 * when an upgrade request arrives.
 *
 * @param server Server instance
 * @param client_fd Client socket file descriptor
 * @param addr Client address
 * @param ssl SSL connection (if any)
 * @return Status code
 */
static int handle_client(ws_server_t *server, int client_fd,
                        const struct sockaddr_storage *addr, SSL *ssl)
{
    /* Large; one per process */
    static http_request_t request;
    char client_addr_str[INET6_ADDRSTRLEN + 8];
    int ret;

    if (!server || client_fd < 0) {
        return WS_EINVAL;
    }

    /* Format client address for logging */
    socket_addr_to_string(addr, sizeof(struct sockaddr_storage),
                         client_addr_str, sizeof(client_addr_str));

    /* Generate unique client ID using PID and timestamp for forked processes */
    static uint32_t process_counter = 0;
    uint32_t client_id = ((uint32_t)getpid() << 16) | (__sync_fetch_and_add(&process_counter, 1) & 0xFFFF);

    log_info("[Client %u] New connection from %s", client_id, client_addr_str);

    for (int served = 0; ; served++) {
        /* Between requests, wait only briefly for the next one */
        if (served > 0) {
            socket_set_timeout(client_fd, HTTP_KEEPALIVE_TIMEOUT);
        }

        ret = server_recv_request(client_fd, ssl, &request);
        if (ret != WS_SUCCESS) {
            if (request.error_status) {
                http_send_status(client_fd, ssl, request.error_status,
                                 http_status_reason(request.error_status));
                lingering_close(client_fd, ssl);
            }
            return served > 0 ? WS_SUCCESS : ret;
        }

        if (served > 0 && g_config.server.socket_timeout > 0) {
            socket_set_timeout(client_fd, g_config.server.socket_timeout);
        }
        if (served + 1 >= HTTP_KEEPALIVE_MAX_REQUESTS) {
            request.keep_alive = 0;
        }

        if (!check_auth(client_fd, ssl, &request, client_id)) {
            if (!request.keep_alive) {
                return WS_EAUTH;
            }
            continue;
        }

        /* WebSocket upgrade: the connection belongs to the proxy from now on */
        if (http_is_websocket_upgrade(&request)) {
            return handle_websocket(client_fd, ssl, &request, addr, client_id);
        }

        char path[1024];
        if (g_config.metrics_enabled &&
            http_decode_path(request.path, path, sizeof(path)) == 0 &&
            strcmp(path, "/metrics") == 0) {
            serve_metrics(client_fd, ssl, &request);
        } else if (strlen(server->web_root) > 0) {
            if (http_serve_file(client_fd, ssl, server->web_root, &request) < 0) {
                return WS_ESOCKET;
            }
        } else {
            http_send_status(client_fd, ssl, 426, "Upgrade Required");
            return WS_SUCCESS;
        }

        if (!request.keep_alive) {
            return WS_SUCCESS;
        }
    }
}

/**
 * @brief Turn a relative path into an absolute one, in place
 *
 * Daemon mode changes the working directory to "/", so paths given relative
 * to the starting directory must be resolved first.
 *
 * @return 0 on success, -1 if the result does not fit
 */
static int make_absolute(char *path, size_t size)
{
    char cwd[4096];
    char resolved[4096];

    if (path[0] == '\0' || path[0] == '/') {
        return 0;
    }
    if (!getcwd(cwd, sizeof(cwd))) {
        return -1;
    }
    if ((size_t)snprintf(resolved, sizeof(resolved), "%s/%s", cwd, path) >= size) {
        log_error("Path too long: %s/%s", cwd, path);
        return -1;
    }
    strlcpy(path, resolved, size);
    return 0;
}

/**
 * @brief Daemonize the process
 * 
 * @return 0 on success, -1 on failure
 */
static int daemonize(const char *pid_file)
{
    pid_t pid, sid;
    int fd;
    char pid_str[16];
    size_t pid_len;

    /* Fork first time */
    pid = fork();
    if (pid < 0) {
        log_error("fork() failed: %s", strerror(errno));
        return -1;
    }

    if (pid > 0) {
        exit(0);  /* Parent exits */
    }

    /* Create new session */
    sid = setsid();
    if (sid < 0) {
        log_error("setsid() failed: %s", strerror(errno));
        return -1;
    }

    /* Fork second time */
    pid = fork();
    if (pid < 0) {
        log_error("fork() failed: %s", strerror(errno));
        return -1;
    }

    if (pid > 0) {
        exit(0);  /* Parent exits */
    }

    /* Change directory */
    if (chdir("/") < 0) {
        log_error("chdir() failed: %s", strerror(errno));
        return -1;
    }

    /* Redirect stdin, stdout, stderr */
    fd = open("/dev/null", O_RDWR);
    if (fd < 0) {
        log_error("open(/dev/null) failed: %s", strerror(errno));
        return -1;
    }

    if (dup2(fd, STDIN_FILENO) < 0 ||
        dup2(fd, STDOUT_FILENO) < 0 ||
        dup2(fd, STDERR_FILENO) < 0) {
        log_error("dup2() failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (fd > 2) {
        close(fd);
    }

    /* Write PID file if specified */
    if (pid_file && strlen(pid_file) > 0) {
        fd = open(pid_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            log_error("Failed to open PID file: %s", strerror(errno));
            return -1;
        }

        pid_len = snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
        if (write(fd, pid_str, pid_len) < 0) {
            log_error("Failed to write PID file: %s", strerror(errno));
            close(fd);
            return -1;
        }

        close(fd);
    }

    return 0;
}

/**
 * @brief Main entry point
 */
int main(int argc, char *argv[])
{
    logger_config_t logger_config;
    int ret;

    /* Initialize config with defaults */
    if (config_init_defaults(&g_config) != WS_SUCCESS) {
        fprintf(stderr, "Failed to initialize configuration\n");
        return EXIT_FAILURE;
    }

    /* Parse command-line arguments (first pass: validate, find --config) */
    if (config_parse_args(argc, argv, &g_config) != WS_SUCCESS) {
        fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Load configuration file if specified, then re-apply the command line
     * so that command-line options take precedence over the file */
    if (strlen(g_config.config_file) > 0) {
        char config_file[sizeof(g_config.config_file)];
        strlcpy(config_file, g_config.config_file, sizeof(config_file));

        config_init_defaults(&g_config);
        if (config_load_file(config_file, &g_config) != WS_SUCCESS) {
            fprintf(stderr, "Failed to load configuration file\n");
            return EXIT_FAILURE;
        }
        if (config_parse_args(argc, argv, &g_config) != WS_SUCCESS) {
            return EXIT_FAILURE;
        }
    }

    /* Resolve relative paths before anything changes the working directory */
    if (make_absolute(g_config.server.cert_file, sizeof(g_config.server.cert_file)) < 0 ||
        make_absolute(g_config.server.key_file, sizeof(g_config.server.key_file)) < 0 ||
        make_absolute(g_config.web_root, sizeof(g_config.web_root)) < 0 ||
        make_absolute(g_config.token_file, sizeof(g_config.token_file)) < 0 ||
        make_absolute(g_config.auth_file, sizeof(g_config.auth_file)) < 0 ||
        make_absolute(g_config.pid_file, sizeof(g_config.pid_file)) < 0 ||
        make_absolute(g_config.logging.logfile, sizeof(g_config.logging.logfile)) < 0) {
        fprintf(stderr, "Invalid path in configuration\n");
        return EXIT_FAILURE;
    }

    /* Validate configuration */
    if (config_validate(&g_config) != WS_SUCCESS) {
        fprintf(stderr, "Invalid configuration\n");
        return EXIT_FAILURE;
    }

    /* Initialize logging */
    memset(&logger_config, 0, sizeof(logger_config));
    logger_config.level = g_config.logging.level;
    logger_config.targets = g_config.logging.targets;
    logger_config.logfile = strlen(g_config.logging.logfile) > 0 ?
                            g_config.logging.logfile : NULL;
    logger_config.syslog_facility = g_config.logging.syslog_facility;

    if (log_init(&logger_config) != WS_SUCCESS) {
        fprintf(stderr, "Failed to initialize logging\n");
        return EXIT_FAILURE;
    }

    log_info("Starting ws2socket v%s", WS2SOCKET_VERSION);
    config_print(&g_config);

    /* Daemonize if requested */
    if (g_config.daemonize) {
        log_info("Daemonizing...");
        if (daemonize(g_config.pid_file) < 0) {
            log_critical("Failed to daemonize");
            log_shutdown();
            return EXIT_FAILURE;
        }
    }

    /* Load the token file, if token-based routing is enabled */
    if (g_config.token_auth) {
        g_token_auth = token_auth_create(g_config.token_file, 1);
        if (!g_token_auth) {
            log_critical("Failed to initialize token file %s", g_config.token_file);
            log_shutdown();
            return EXIT_FAILURE;
        }
    }

    /* Load the password file, if authentication is enabled */
    if (g_config.auth_file[0]) {
        g_auth = auth_create(g_config.auth_file, g_config.auth_realm);
        if (!g_auth) {
            log_critical("Failed to initialize authentication from %s", g_config.auth_file);
            log_shutdown();
            return EXIT_FAILURE;
        }
        if (!g_config.server.use_ssl) {
            log_warn("Authentication without TLS: passwords are sent in the clear");
        }
    }

    /* Shared counters must exist before the first fork */
    if (metrics_init() != WS_SUCCESS) {
        log_critical("Failed to initialize metrics");
        log_shutdown();
        return EXIT_FAILURE;
    }
    if (g_config.metrics_enabled && !g_auth) {
        log_warn("/metrics is enabled without authentication");
    }

    /* Initialize proxy */
    if (proxy_init(&g_config.proxy) != WS_SUCCESS) {
        log_critical("Failed to initialize proxy");
        log_shutdown();
        return EXIT_FAILURE;
    }

    /* Create server */
    g_server = server_create();
    if (!g_server) {
        log_critical("Failed to create server");
        log_shutdown();
        return EXIT_FAILURE;
    }

    /* Initialize server */
    if (server_init(g_server, &g_config.server) != WS_SUCCESS) {
        log_critical("Failed to initialize server");
        server_destroy(g_server);
        log_shutdown();
        return EXIT_FAILURE;
    }

    /* Set web root if specified */
    if (strlen(g_config.web_root) > 0) {
        strlcpy(g_server->web_root, g_config.web_root, sizeof(g_server->web_root));
        log_info("Web root directory: %s", g_server->web_root);
    }

    /* Start listening */
    if (server_listen(g_server) != WS_SUCCESS) {
        log_critical("Failed to start listening");
        server_destroy(g_server);
        log_shutdown();
        return EXIT_FAILURE;
    }

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Main server loop */
    log_info("Entering main server loop");
    ret = server_run(g_server, handle_client);

    /* Cleanup */
    log_info("Cleaning up...");
    proxy_shutdown();
    server_destroy(g_server);
    g_server = NULL;
    log_shutdown();

    return (ret == WS_SUCCESS) ? EXIT_SUCCESS : EXIT_FAILURE;
}
