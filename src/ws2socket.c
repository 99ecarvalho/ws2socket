/**
 * @file ws2socket.c
 * @brief Main WebSocket to TCP Socket Proxy Application
 * @author Eduardo Correia <ecorreia@apliant.com.br>
 * 
 * Entry point for the ws2socket application.
 * Handles server initialization, client connections, and shutdown.
 * 
 * License: LGPL v3
 */

#include "common.h"
#include "config.h"
#include "logging.h"
#include "server.h"
#include "proxy.h"
#include "websocket.h"
#include "utils.h"
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
 * @brief Handle client WebSocket connection
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
    websocket_t *ws = NULL;
    proxy_client_t *proxy_client = NULL;
    http_request_t request;
    char client_addr_str[INET_ADDRSTRLEN + 6];
    int ret;

    if (!server || client_fd < 0) {
        return WS_EINVAL;
    }

    /* Format client address for logging */
    socket_addr_to_string(addr, sizeof(struct sockaddr_storage),
                         client_addr_str, sizeof(client_addr_str));

    /* Generate unique client ID using PID and timestamp for forked processes */
    static uint32_t process_counter = 0;
    uint32_t client_id = (getpid() << 16) | (__sync_fetch_and_add(&process_counter, 1) & 0xFFFF);

    log_info("[Client %u] New connection from %s", client_id, client_addr_str);

    /* Receive HTTP request */
    ret = server_recv_request(client_fd, &request);
    if (ret != WS_SUCCESS) {
        log_error("Failed to receive HTTP request");
        close(client_fd);
        return ret;
    }

    /* Declare external functions from http_server.c */
    extern int http_is_websocket_upgrade(const http_request_t *request);
    extern int http_serve_file(int client_fd, const char *web_root, const char *uri_path);
    extern const char *http_get_header_value(const http_request_t *request, const char *name);

    /* Check if this is a WebSocket upgrade request */
    if (!http_is_websocket_upgrade(&request)) {
        /* Serve static file if web_root is set */
        if (strlen(server->web_root) > 0) {
            log_info("[Client %u] Serving file: %s", client_id, request.path);
            http_serve_file(client_fd, server->web_root, request.path);
        } else {
            const char *response = "HTTP/1.1 426 Upgrade Required\r\n\r\n";
            socket_send(client_fd, (uint8_t *)response, strlen(response), 0);
        }
        close(client_fd);
        return WS_SUCCESS;
    }

    /* Create WebSocket */
    ws = websocket_create();
    if (!ws) {
        log_error("Failed to create WebSocket");
        close(client_fd);
        return WS_ENOMEM;
    }

    /* Initialize WebSocket */
    ret = websocket_init(ws, client_fd, ssl);
    if (ret != WS_SUCCESS) {
        log_error("Failed to initialize WebSocket: %d", ret);
        websocket_destroy(ws);
        close(client_fd);
        return ret;
    }

    /* Perform WebSocket handshake */
    const char *ws_key = http_get_header_value(&request, "Sec-WebSocket-Key");
    if (!ws_key) {
        log_error("Missing Sec-WebSocket-Key header");
        websocket_destroy(ws);
        close(client_fd);
        return WS_EPROTO;
    }

    /* Build headers array for websocket_accept */
    const char *headers[MAX_HTTP_HEADERS];
    static char header_lines[MAX_HTTP_HEADERS][640];
    for (int i = 0; i < request.num_headers && i < MAX_HTTP_HEADERS; i++) {
        /* Safely format header line with explicit null termination */
        int written = snprintf(header_lines[i], sizeof(header_lines[i]), "%s: %s",
                              request.headers[i].name, request.headers[i].value);
        if (written >= (int)sizeof(header_lines[i])) {
            /* Truncation occurred - ensure null termination */
            header_lines[i][sizeof(header_lines[i]) - 1] = '\0';
        }
        headers[i] = header_lines[i];
    }

    ret = websocket_accept(ws, headers, request.num_headers);
    if (ret != WS_SUCCESS) {
        log_error("Failed to accept WebSocket: %d", ret);
        websocket_destroy(ws);
        close(client_fd);
        return ret;
    }

    /* Create proxy client */
    proxy_client = proxy_client_create();
    if (!proxy_client) {
        log_error("[Client %u] Failed to create proxy client", client_id);
        websocket_destroy(ws);
        close(client_fd);
        return WS_ENOMEM;
    }

    proxy_client->ws = ws;
    proxy_client->client_id = client_id;
    memcpy(&proxy_client->src_addr, addr, sizeof(struct sockaddr_storage));
    proxy_client->src_addr_len = sizeof(struct sockaddr_storage);

    /* Connect to target server */
    char target_host[256];
    uint16_t target_port;
    
    // Use target from global config (set via command line or config file)
    if (parse_hostport(g_config.target_host, target_host, 
                      sizeof(target_host), &target_port) != 0) {
        log_error("Failed to parse target server");
        proxy_client_destroy(proxy_client);
        return WS_EINVAL;
    }
    
    // If no port specified, use target_port
    if (target_port == 0) {
        target_port = g_config.target_port;
    }

    ret = proxy_connect_target(proxy_client, target_host, target_port, g_config.server.socket_timeout);
    if (ret != WS_SUCCESS) {
        log_error("[Client %u] Failed to connect to target %s:%u", client_id, target_host, target_port);
        proxy_client_destroy(proxy_client);
        return ret;
    }

    log_info("[Client %u] WebSocket connection established, proxying to %s:%u", 
             client_id, target_host, target_port);

    /* Start bidirectional proxy */
    proxy_forward(proxy_client);

    /* Cleanup */
    proxy_client_destroy(proxy_client);

    return WS_SUCCESS;
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

    /* Parse command-line arguments */
    if (config_parse_args(argc, argv, &g_config) != WS_SUCCESS) {
        fprintf(stderr, "Invalid arguments\n");
        config_print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    /* Load configuration file if specified */
    if (strlen(g_config.config_file) > 0) {
        if (config_load_file(g_config.config_file, &g_config) != WS_SUCCESS) {
            fprintf(stderr, "Failed to load configuration file\n");
            return EXIT_FAILURE;
        }
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

    log_info("Starting ws2socket v0.1.0");
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
