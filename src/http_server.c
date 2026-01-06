/**
 * @file http_server.c
 * @brief HTTP server with static file serving and WebSocket upgrade
 * @author Eduardo Correia <ecorreia@apliant.com.br>
 * 
 * HTTP/1.1 server supporting:
 * - Static file serving
 * - WebSocket upgrade
 * - MIME type detection
 * 
 * License: LGPL v3
 */

#include "server.h"
#include "utils.h"
#include "logging.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>

/**
 * @brief Parse HTTP request line and headers
 */
int http_parse_request(const char *request_data, size_t request_len,
                      http_request_t *request_out)
{
    if (!request_data || !request_out) {
        return -1;
    }
    
    memset(request_out, 0, sizeof(http_request_t));
    
    // Parse request line: METHOD /path HTTP/1.1
    const char *line_end = strstr(request_data, "\r\n");
    if (!line_end) {
        return -1;
    }
    
    // Extract method
    const char *p = request_data;
    const char *space = strchr(p, ' ');
    if (!space || space > line_end) {
        return -1;
    }
    
    size_t method_len = space - p;
    if (method_len >= sizeof(request_out->method)) {
        method_len = sizeof(request_out->method) - 1;
    }
    memcpy(request_out->method, p, method_len);
    request_out->method[method_len] = '\0';
    
    // Extract path
    p = space + 1;
    space = strchr(p, ' ');
    if (!space || space > line_end) {
        return -1;
    }
    
    size_t path_len = space - p;
    if (path_len >= sizeof(request_out->path)) {
        path_len = sizeof(request_out->path) - 1;
    }
    memcpy(request_out->path, p, path_len);
    request_out->path[path_len] = '\0';
    
    // Extract version
    p = space + 1;
    size_t version_len = line_end - p;
    if (version_len >= sizeof(request_out->version)) {
        version_len = sizeof(request_out->version) - 1;
    }
    memcpy(request_out->version, p, version_len);
    request_out->version[version_len] = '\0';
    
    // Parse headers
    p = line_end + 2;  // Skip \r\n
    request_out->num_headers = 0;
    
    while (p < request_data + request_len) {
        line_end = strstr(p, "\r\n");
        if (!line_end) {
            break;
        }
        
        // Empty line marks end of headers
        if (line_end == p) {
            break;
        }
        
        // Find colon separator
        const char *colon = strchr(p, ':');
        if (!colon || colon > line_end) {
            p = line_end + 2;
            continue;
        }
        
        if (request_out->num_headers < MAX_HTTP_HEADERS) {
            // Extract header name
            size_t name_len = colon - p;
            if (name_len >= sizeof(request_out->headers[0].name)) {
                name_len = sizeof(request_out->headers[0].name) - 1;
            }
            memcpy(request_out->headers[request_out->num_headers].name, p, name_len);
            request_out->headers[request_out->num_headers].name[name_len] = '\0';
            
            // Extract header value (skip leading whitespace)
            const char *value = colon + 1;
            while (value < line_end && (*value == ' ' || *value == '\t')) {
                value++;
            }
            
            size_t value_len = line_end - value;
            if (value_len >= sizeof(request_out->headers[0].value)) {
                value_len = sizeof(request_out->headers[0].value) - 1;
            }
            memcpy(request_out->headers[request_out->num_headers].value, value, value_len);
            request_out->headers[request_out->num_headers].value[value_len] = '\0';
            
            request_out->num_headers++;
        }
        
        p = line_end + 2;
    }
    
    return 0;
}

/**
 * @brief Get header value from request
 */
const char *http_get_header_value(const http_request_t *request, const char *name)
{
    if (!request || !name) {
        return NULL;
    }
    
    for (int i = 0; i < request->num_headers; i++) {
        if (strcasecmp(request->headers[i].name, name) == 0) {
            return request->headers[i].value;
        }
    }
    
    return NULL;
}

/**
 * @brief Check if request is WebSocket upgrade
 */
int http_is_websocket_upgrade(const http_request_t *request)
{
    if (!request) {
        return 0;
    }
    
    const char *upgrade = http_get_header_value(request, "Upgrade");
    const char *connection = http_get_header_value(request, "Connection");
    const char *ws_key = http_get_header_value(request, "Sec-WebSocket-Key");
    
    return (upgrade && strcasecmp(upgrade, "websocket") == 0) &&
           (connection && strstr(connection, "Upgrade")) &&
           (ws_key != NULL);
}

/**
 * @brief Get MIME type from file extension
 */
const char *http_get_mime_type(const char *path)
{
    if (!path) {
        return "application/octet-stream";
    }
    
    const char *ext = strrchr(path, '.');
    if (!ext) {
        return "application/octet-stream";
    }
    
    ext++;  // Skip the dot
    
    if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0) {
        return "text/html";
    } else if (strcasecmp(ext, "css") == 0) {
        return "text/css";
    } else if (strcasecmp(ext, "js") == 0) {
        return "application/javascript";
    } else if (strcasecmp(ext, "json") == 0) {
        return "application/json";
    } else if (strcasecmp(ext, "png") == 0) {
        return "image/png";
    } else if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) {
        return "image/jpeg";
    } else if (strcasecmp(ext, "gif") == 0) {
        return "image/gif";
    } else if (strcasecmp(ext, "svg") == 0) {
        return "image/svg+xml";
    } else if (strcasecmp(ext, "ico") == 0) {
        return "image/x-icon";
    } else if (strcasecmp(ext, "wasm") == 0) {
        return "application/wasm";
    } else if (strcasecmp(ext, "txt") == 0) {
        return "text/plain";
    }
    
    return "application/octet-stream";
}

/**
 * @brief Serve static file
 */
int http_serve_file(int client_fd, const char *web_root, const char *uri_path)
{
    if (!web_root || !uri_path) {
        return -1;
    }
    
    // Strip query string from path (if present)
    char path_only[1024];
    strncpy(path_only, uri_path, sizeof(path_only) - 1);
    path_only[sizeof(path_only) - 1] = '\0';
    
    // Find and remove query string
    char *query_start = strchr(path_only, '?');
    if (query_start) {
        *query_start = '\0';
    }
    
    // Build full file path
    char file_path[1024];
    snprintf(file_path, sizeof(file_path), "%s%s", web_root, path_only);
    
    // If path ends with /, append index.html
    size_t len = strlen(file_path);
    if (len > 0 && file_path[len-1] == '/') {
        strncat(file_path, "index.html", sizeof(file_path) - len - 1);
    }
    
    // Security: prevent directory traversal
    if (strstr(file_path, "..")) {
        log_warn("Directory traversal attempt: %s", file_path);
        const char *response = "HTTP/1.1 403 Forbidden\r\n\r\n";
        socket_send(client_fd, (uint8_t *)response, strlen(response), 0);
        return -1;
    }
    
    // Open file
    int fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        log_debug("File not found: %s", file_path);
        const char *response = "HTTP/1.1 404 Not Found\r\n"
                              "Content-Type: text/html\r\n\r\n"
                              "<h1>404 Not Found</h1>";
        socket_send(client_fd, (uint8_t *)response, strlen(response), 0);
        return -1;
    }
    
    // Get file size
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        const char *response = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
        socket_send(client_fd, (uint8_t *)response, strlen(response), 0);
        return -1;
    }
    
    // Get MIME type
    const char *mime = http_get_mime_type(file_path);
    
    // Send headers
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "\r\n",
        mime, (long)st.st_size);
    
    if (socket_send(client_fd, (uint8_t *)header, header_len, 0) != header_len) {
        close(fd);
        return -1;
    }
    
    // Send file content
    char buffer[8192];
    ssize_t nread;
    while ((nread = read(fd, buffer, sizeof(buffer))) > 0) {
        if (socket_send(client_fd, (uint8_t *)buffer, nread, 0) != nread) {
            close(fd);
            return -1;
        }
    }
    
    close(fd);
    log_info("Served file: %s (%ld bytes)", file_path, (long)st.st_size);
    return 0;
}
