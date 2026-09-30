/**
 * @file http_server.c
 * @brief HTTP server with static file serving and WebSocket upgrade
 * @author Eduardo Correia <ecorreia@apliant.com.br>
 *
 * HTTP/1.1 server supporting:
 * - Static file serving (GET and HEAD) with keep-alive
 * - Percent-decoded paths with traversal protection
 * - Conditional requests (If-Modified-Since) and byte ranges
 * - WebSocket upgrade detection
 * - MIME type detection
 *
 * @copyright Copyright (c) 2026 Eduardo Correia <ecorreia@apliant.com.br>
 *
 * This file is part of ws2socket. It is free software, licensed under the
 * GNU Lesser General Public License v3.0 or later. See COPYING.LESSER
 * and COPYING for details.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#define _GNU_SOURCE  /* strcasestr, timegm */
#include "server.h"
#include "utils.h"
#include "logging.h"
#include "metrics.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

/**
 * @brief Parse HTTP request line and headers
 *
 * On failure, request_out->error_status holds the status to answer with.
 */
int http_parse_request(const char *request_data, size_t request_len,
                      http_request_t *request_out)
{
    if (!request_data || !request_out) {
        return -1;
    }

    memset(request_out, 0, sizeof(http_request_t));
    request_out->error_status = 400;

    // Parse request line: METHOD /path HTTP/1.1
    const char *line_end = strstr(request_data, "\r\n");
    if (!line_end) {
        return -1;
    }

    // Extract method
    const char *p = request_data;
    const char *space = memchr(p, ' ', line_end - p);
    if (!space || space == p) {
        return -1;
    }

    size_t method_len = space - p;
    if (method_len >= sizeof(request_out->method)) {
        return -1;
    }
    memcpy(request_out->method, p, method_len);
    request_out->method[method_len] = '\0';

    // Extract request target
    p = space + 1;
    space = memchr(p, ' ', line_end - p);
    if (!space || space == p) {
        return -1;
    }

    size_t path_len = space - p;
    if (path_len >= sizeof(request_out->path)) {
        request_out->error_status = 414;
        return -1;
    }
    memcpy(request_out->path, p, path_len);
    request_out->path[path_len] = '\0';

    // The target must be an absolute path without control characters
    if (request_out->path[0] != '/') {
        return -1;
    }
    for (size_t i = 0; i < path_len; i++) {
        if ((unsigned char)request_out->path[i] < 0x21 ||
            (unsigned char)request_out->path[i] == 0x7F) {
            return -1;
        }
    }

    // Extract version
    p = space + 1;
    size_t version_len = line_end - p;
    if (version_len >= sizeof(request_out->version) ||
        strncmp(p, "HTTP/1.", 7) != 0) {
        return -1;
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
        const char *colon = memchr(p, ':', line_end - p);
        if (!colon || colon == p) {
            return -1;
        }

        if (request_out->num_headers >= MAX_HTTP_HEADERS) {
            request_out->error_status = 431;
            return -1;
        }

        http_header_t *h = &request_out->headers[request_out->num_headers];

        // Extract header name
        size_t name_len = colon - p;
        if (name_len >= sizeof(h->name)) {
            request_out->error_status = 431;
            return -1;
        }
        memcpy(h->name, p, name_len);
        h->name[name_len] = '\0';

        // Extract header value (skip surrounding whitespace)
        const char *value = colon + 1;
        const char *value_end = line_end;
        while (value < value_end && (*value == ' ' || *value == '\t')) {
            value++;
        }
        while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t')) {
            value_end--;
        }

        size_t value_len = value_end - value;
        if (value_len >= sizeof(h->value)) {
            request_out->error_status = 431;
            return -1;
        }
        memcpy(h->value, value, value_len);
        h->value[value_len] = '\0';

        request_out->num_headers++;
        p = line_end + 2;
    }

    // Keep-alive is the default in HTTP/1.1 and opt-in in HTTP/1.0
    const char *connection = http_get_header_value(request_out, "Connection");
    if (strcmp(request_out->version, "HTTP/1.1") == 0) {
        request_out->keep_alive = !(connection && strcasestr(connection, "close"));
    } else {
        request_out->keep_alive = connection && strcasestr(connection, "keep-alive");
    }

    // This server does not read request bodies, so it cannot keep a
    // connection whose next request would start after an unread body
    const char *content_length = http_get_header_value(request_out, "Content-Length");
    if ((content_length && strcmp(content_length, "0") != 0) ||
        http_get_header_value(request_out, "Transfer-Encoding")) {
        request_out->keep_alive = 0;
    }

    request_out->error_status = 0;
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

    // Header values are case-insensitive ("Upgrade", "upgrade", ...)
    return (upgrade && strcasecmp(upgrade, "websocket") == 0) &&
           (connection && strcasestr(connection, "upgrade")) &&
           (ws_key != NULL);
}

/**
 * @brief Get MIME type from file extension
 */
const char *http_get_mime_type(const char *path)
{
    static const struct {
        const char *ext;
        const char *type;
    } types[] = {
        { "html", "text/html; charset=utf-8" },
        { "htm",  "text/html; charset=utf-8" },
        { "css",  "text/css; charset=utf-8" },
        { "js",   "text/javascript; charset=utf-8" },
        { "mjs",  "text/javascript; charset=utf-8" },
        { "json", "application/json" },
        { "map",  "application/json" },
        { "webmanifest", "application/manifest+json" },
        { "txt",  "text/plain; charset=utf-8" },
        { "xml",  "application/xml" },
        { "png",  "image/png" },
        { "jpg",  "image/jpeg" },
        { "jpeg", "image/jpeg" },
        { "gif",  "image/gif" },
        { "svg",  "image/svg+xml" },
        { "ico",  "image/x-icon" },
        { "webp", "image/webp" },
        { "wasm", "application/wasm" },
        { "mp3",  "audio/mpeg" },
        { "oga",  "audio/ogg" },
        { "ogg",  "audio/ogg" },
        { "wav",  "audio/wav" },
        { "woff", "font/woff" },
        { "woff2", "font/woff2" },
        { "ttf",  "font/ttf" },
        { "otf",  "font/otf" },
    };

    if (!path) {
        return "application/octet-stream";
    }

    const char *slash = strrchr(path, '/');
    const char *ext = strrchr(path, '.');
    if (!ext || (slash && ext < slash)) {
        return "application/octet-stream";
    }
    ext++;  // Skip the dot

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        if (strcasecmp(ext, types[i].ext) == 0) {
            return types[i].type;
        }
    }

    return "application/octet-stream";
}

/**
 * @brief Reason phrase for a status code
 */
const char *http_status_reason(int code)
{
    switch (code) {
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 414: return "URI Too Long";
        case 416: return "Range Not Satisfiable";
        case 426: return "Upgrade Required";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
    }
}

/**
 * @brief Format a time as an HTTP date (RFC 9110 IMF-fixdate)
 */
static void http_format_date(time_t t, char *out, size_t out_size)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, out_size, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

/**
 * @brief Parse an HTTP date (IMF-fixdate only)
 *
 * @return Unix time, or -1 if the value is not a valid date
 */
static time_t http_parse_date(const char *value)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    const char *end = strptime(value, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    if (!end || *end != '\0') {
        return -1;
    }
    return timegm(&tm);
}

/**
 * @brief Send a complete HTTP response
 */
int http_respond(int client_fd, SSL *ssl, const http_request_t *req, int code,
                 const char *extra_headers, const char *content_type,
                 const void *body, size_t body_len)
{
    char date[64];
    char header[2048];
    int keep_alive = req ? req->keep_alive : 0;
    int is_head = req && strcmp(req->method, "HEAD") == 0;

    http_format_date(time(NULL), date, sizeof(date));

    int len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Date: %s\r\n"
        "Server: ws2socket/%s\r\n"
        "%s%s%s"
        "Content-Length: %zu\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "%s"
        "Connection: %s\r\n"
        "\r\n",
        code, http_status_reason(code), date, WS2SOCKET_VERSION,
        content_type ? "Content-Type: " : "",
        content_type ? content_type : "",
        content_type ? "\r\n" : "",
        body_len,
        extra_headers ? extra_headers : "",
        keep_alive ? "keep-alive" : "close");

    if (len < 0 || (size_t)len >= sizeof(header)) {
        return WS_EINVAL;
    }

    metrics_http_response(code);

    if (io_send_all(client_fd, ssl, (uint8_t *)header, (size_t)len) != len) {
        return WS_ESOCKET;
    }
    if (!is_head && body && body_len > 0 &&
        io_send_all(client_fd, ssl, body, body_len) != (ssize_t)body_len) {
        return WS_ESOCKET;
    }
    return WS_SUCCESS;
}

/**
 * @brief Send a bare HTTP status response and close the connection
 */
int http_send_status(int client_fd, SSL *ssl, int code, const char *reason)
{
    char body[128];
    int len = snprintf(body, sizeof(body), "%s\n", reason);
    return http_respond(client_fd, ssl, NULL, code, NULL,
                        "text/plain; charset=utf-8", body, (size_t)len);
}

/**
 * @brief Send a short plain-text error that respects keep-alive
 */
static int http_respond_error(int client_fd, SSL *ssl, const http_request_t *req,
                              int code, const char *extra_headers)
{
    char body[128];
    int len = snprintf(body, sizeof(body), "%s\n", http_status_reason(code));
    return http_respond(client_fd, ssl, req, code, extra_headers,
                        "text/plain; charset=utf-8", body, (size_t)len);
}

/**
 * @brief Percent-decode the path part of a request target
 *
 * Stops at '?' or '#'. Rejects malformed escapes and encoded NUL bytes.
 *
 * @return 0 on success, -1 if the path is invalid
 */
int http_decode_path(const char *target, char *out, size_t out_size)
{
    size_t j = 0;

    for (size_t i = 0; target[i] && target[i] != '?' && target[i] != '#'; i++) {
        char c = target[i];

        if (c == '%') {
            if (!isxdigit((unsigned char)target[i + 1]) ||
                !isxdigit((unsigned char)target[i + 2])) {
                return -1;
            }
            char hex[3] = { target[i + 1], target[i + 2], '\0' };
            c = (char)strtol(hex, NULL, 16);
            if (c == '\0') {
                return -1;
            }
            i += 2;
        }

        if (j + 1 >= out_size) {
            return -1;
        }
        out[j++] = c;
    }

    out[j] = '\0';
    return 0;
}

/**
 * @brief Check that a decoded path has no "." or ".." segments
 */
static int http_path_is_safe(const char *path)
{
    const char *segment = path;

    while (*segment) {
        while (*segment == '/') {
            segment++;
        }
        size_t len = strcspn(segment, "/");
        if ((len == 1 && segment[0] == '.') ||
            (len == 2 && segment[0] == '.' && segment[1] == '.')) {
            return 0;
        }
        segment += len;
    }
    return 1;
}

/**
 * @brief Parse a single "bytes=" range against a file size
 *
 * @return 1 for a valid range (start/end set), 0 to ignore the header and
 *         send the whole file, -1 if the range cannot be satisfied
 */
static int http_parse_range(const char *value, off_t size, off_t *start, off_t *end)
{
    char *endp;

    if (strncmp(value, "bytes=", 6) != 0 || strchr(value, ',')) {
        return 0;  // Other units and multiple ranges: serve the whole file
    }
    value += 6;

    if (*value == '-') {
        // Suffix range: the last N bytes
        long long suffix = strtoll(value + 1, &endp, 10);
        if (endp == value + 1 || *endp != '\0' || suffix < 0) {
            return 0;
        }
        if (suffix == 0 || size == 0) {
            return -1;
        }
        *start = (suffix >= size) ? 0 : size - suffix;
        *end = size - 1;
        return 1;
    }

    long long first = strtoll(value, &endp, 10);
    if (endp == value || *endp != '-' || first < 0) {
        return 0;
    }
    const char *second = endp + 1;
    long long last = size - 1;
    if (*second != '\0') {
        last = strtoll(second, &endp, 10);
        if (*endp != '\0' || last < first) {
            return 0;
        }
    }

    if (first >= size) {
        return -1;
    }
    *start = first;
    *end = (last >= size) ? size - 1 : last;
    return 1;
}

/**
 * @brief Serve static file
 */
int http_serve_file(int client_fd, SSL *ssl, const char *web_root,
                    const http_request_t *req)
{
    char decoded[1024];
    char file_path[2048];
    char extra[768];
    struct stat st;

    if (!web_root || !req) {
        return -1;
    }

    if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "HEAD") != 0) {
        http_respond_error(client_fd, ssl, req, 405, "Allow: GET, HEAD\r\n");
        return 405;
    }

    if (http_decode_path(req->path, decoded, sizeof(decoded)) != 0) {
        http_respond_error(client_fd, ssl, req, 400, NULL);
        return 400;
    }

    // Security: prevent directory traversal
    if (!http_path_is_safe(decoded)) {
        log_warn("Directory traversal attempt: %s", req->path);
        http_respond_error(client_fd, ssl, req, 403, NULL);
        return 403;
    }

    if ((size_t)snprintf(file_path, sizeof(file_path), "%s%s", web_root, decoded)
            >= sizeof(file_path)) {
        http_respond_error(client_fd, ssl, req, 414, NULL);
        return 414;
    }

    if (stat(file_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        size_t len = strlen(decoded);
        if (len == 0 || decoded[len - 1] != '/') {
            // Directory without trailing slash: redirect so relative links work
            const char *query = strchr(req->path, '?');
            size_t path_len = strcspn(req->path, "?#");
            snprintf(extra, sizeof(extra), "Location: %.*s/%s\r\n",
                     (int)path_len, req->path, query ? query : "");
            http_respond_error(client_fd, ssl, req, 301, extra);
            return 301;
        }
        strncat(file_path, "index.html", sizeof(file_path) - strlen(file_path) - 1);
    }

    int fd = open(file_path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0) {
            close(fd);
        }
        log_debug("File not found: %s", file_path);
        http_respond_error(client_fd, ssl, req, 404, NULL);
        return 404;
    }

    char last_modified[64];
    http_format_date(st.st_mtime, last_modified, sizeof(last_modified));

    // Conditional request: the browser's copy is still current
    const char *ims = http_get_header_value(req, "If-Modified-Since");
    const char *range = http_get_header_value(req, "Range");
    if (ims && !range) {
        time_t since = http_parse_date(ims);
        if (since != -1 && st.st_mtime <= since) {
            close(fd);
            snprintf(extra, sizeof(extra),
                     "Last-Modified: %s\r\nCache-Control: no-cache\r\n", last_modified);
            http_respond(client_fd, ssl, req, 304, extra, NULL, NULL, 0);
            return 304;
        }
    }

    // Byte ranges (ignored when If-Range is present, which is always safe)
    off_t start = 0;
    off_t end = st.st_size - 1;
    int code = 200;
    if (range && !http_get_header_value(req, "If-Range")) {
        int r = http_parse_range(range, st.st_size, &start, &end);
        if (r < 0) {
            close(fd);
            snprintf(extra, sizeof(extra), "Content-Range: bytes */%lld\r\n",
                     (long long)st.st_size);
            http_respond_error(client_fd, ssl, req, 416, extra);
            return 416;
        }
        if (r > 0) {
            code = 206;
        }
    }
    off_t length = (st.st_size == 0) ? 0 : end - start + 1;

    // Send headers
    char header[1536];
    char date[64];
    http_format_date(time(NULL), date, sizeof(date));
    char content_range[96] = "";
    if (code == 206) {
        snprintf(content_range, sizeof(content_range),
                 "Content-Range: bytes %lld-%lld/%lld\r\n",
                 (long long)start, (long long)end, (long long)st.st_size);
    }

    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Date: %s\r\n"
        "Server: ws2socket/%s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lld\r\n"
        "%s"
        "Last-Modified: %s\r\n"
        "Cache-Control: no-cache\r\n"
        "Accept-Ranges: bytes\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Connection: %s\r\n"
        "\r\n",
        code, http_status_reason(code), date, WS2SOCKET_VERSION,
        http_get_mime_type(file_path), (long long)length, content_range,
        last_modified, req->keep_alive ? "keep-alive" : "close");

    metrics_http_response(code);
    if (io_send_all(client_fd, ssl, (uint8_t *)header, header_len) != header_len) {
        close(fd);
        return -1;
    }

    // Send file content (not for HEAD)
    if (strcmp(req->method, "HEAD") != 0 && length > 0) {
        char buffer[16384];
        off_t remaining = length;

        if (lseek(fd, start, SEEK_SET) < 0) {
            close(fd);
            return -1;
        }
        while (remaining > 0) {
            size_t chunk = remaining > (off_t)sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
            ssize_t nread = read(fd, buffer, chunk);
            if (nread < 0 && errno == EINTR) {
                continue;
            }
            if (nread <= 0 ||
                io_send_all(client_fd, ssl, (uint8_t *)buffer, (size_t)nread) != nread) {
                close(fd);
                return -1;
            }
            remaining -= nread;
        }
    }

    close(fd);
    log_info("Served %s %s (%d, %lld bytes)", req->method, file_path, code,
             (long long)length);
    return code;
}
