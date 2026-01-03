# ws2socket with noVNC Integration Guide

## Overview

ws2socket is with complete WebSocket protocol support and HTTP static file serving, making it perfect for noVNC deployments.

## What's Implemented

### ✅ Complete Features

1. **WebSocket Protocol (RFC 6455)**
   - SHA1 + Base64 handshake
   - Frame encoding/decoding (all payload sizes)
   - Client-to-server masking/unmasking
   - Control frames: PING, PONG, CLOSE
   - Binary and text frames

2. **HTTP Server**
   - Complete HTTP/1.1 request parsing
   - Static file serving with proper MIME types
   - Support for HTML, CSS, JavaScript, PNG, WASM, and more
   - Directory traversal protection
   - WebSocket upgrade detection

3. **Bidirectional Proxy**
   - select()-based event loop
   - WebSocket ↔ TCP forwarding
   - Statistics tracking (bytes sent/received)
   - Configurable buffer sizes and timeouts

4. **Configuration**
   - Command-line argument parsing
   - Web root directory support
   - SSL/TLS certificate options (stub - needs implementation)
   - Logging configuration
   - Daemonization support

## Quick Start with noVNC

### 1. Download noVNC

```bash
git clone https://github.com/novnc/noVNC.git ./noVNC
```

### 2. Build ws2socket

```bash
cd /dados/ws2tcp/ws2socket
mkdir -p build && cd build
cmake ..
make
```

### 3. Start a VNC Server

If you don't have one running, start Xvnc or x11vnc:

```bash
# Example with Xvnc on display :1 (port 5901)
Xvnc :1 -geometry 1280x720 -depth 24
```

If you prefer x11vnc
```bash
# Example with DISPLAY=172.23.128.1:4.0 (default port 5900)
x11vnc -display $DISPLAY -nopw -noshm -localhost -forever -shared

# Example with DISPLAY=172.23.128.1:4.0 (default port 5900) with password
mkdir -p ~/.vnc
x11vnc -storepasswd ~/.vnc/passwd
chmod 600 ~/.vnc/passwd
x11vnc -display $DISPLAY -nopw -noshm -localhost -forever -shared -rfbauth ~/.vnc/passwd
```

### 4. Run ws2socket

```bash
./build/ws2socket \
  --listen 0.0.0.0:6080 \
  --target 127.0.0.1:5901 \
  --web-root /path/to/novnc \
  --verbose
```

**Note**: The `--web-root` option may need to be added to the command-line parser. See the "Adding web-root Option" section below.

### 5. Access noVNC

Open your browser to:
```
http://your-server:6080/vnc.html?host=your-server&port=6080
```

Or with auto-connect:
```
http://your-server:6080/vnc.html?host=your-server&port=6080&autoconnect=1
```

## How It Works

1. **HTTP Request**: Browser requests `/vnc.html` → ws2socket serves from web_root
2. **WebSocket Upgrade**: Browser sends WebSocket upgrade for `/websockify` → ws2socket performs handshake
3. **VNC Connection**: ws2socket connects to VNC server (127.0.0.1:5901)
4. **Bidirectional Proxy**: ws2socket forwards:
   - WebSocket frames → TCP to VNC server
   - TCP from VNC server → WebSocket frames to browser

## noVNC URL Parameters

The noVNC application supports URL parameters for controlling behavior and pre-configuring connections. Parameters can be passed as query strings or fragments.

### Connection Parameters

- **`host`** - The WebSocket host to connect to (deprecated, use `path`)
- **`port`** - The WebSocket port to connect to (deprecated, use `path`)
- **`path`** - The WebSocket URL (preferred method)
- **`encrypt`** - Use TLS for WebSocket connection (deprecated, use `path`)

### Auto-connect & Reconnection

- **`autoconnect`** - Automatically connect as soon as the page loads (0 or 1)
  - Example: `?autoconnect=1`
- **`reconnect`** - Auto-reconnect if connection drops (default: true)
- **`reconnect_delay`** - Milliseconds to wait before reconnecting

### Session Control

- **`password`** - Password for the VNC server
- **`shared`** - Allow multiple clients to connect simultaneously (0 or 1)
- **`view_only`** - Read-only mode, disables keyboard and mouse input (0 or 1)
- **`repeaterID`** - VNC repeater ID if using a repeater proxy

### Display Options

- **`view_clip`** - Clip display to window or use scrollbars
- **`resize`** - How to resize remote session: `off`, `scale`, or `remote`
- **`quality`** - JPEG quality level (0-9, default varies)
- **`compression`** - Compression level (0-9, default varies)

### Other Options

- **`bell`** - Enable/disable keyboard bell sounds (0 or 1)
- **`logging`** - Console log level: `error`, `warn`, `info`, or `debug`

### Usage Examples

**Basic connection with auto-connect:**
```
http://localhost:6082/vnc.html?host=localhost&port=6082&autoconnect=1
```

**With multiple options:**
```
http://localhost:6082/vnc.html?host=localhost&port=6082&autoconnect=1&shared=1&quality=8
```

**Using fragment (not sent to server):**
```
http://localhost:6082/vnc.html#host=localhost&port=6082&autoconnect=1&view_only=0
```

**With password:**
```
http://localhost:6082/vnc.html?host=localhost&port=6082&autoconnect=1&password=MyPassword
```

### Configuration Files

Parameters can also be set in configuration files:

- **`defaults.json`** - Default settings (user can override)
- **`mandatory.json`** - Mandatory settings (user cannot change)

Example `defaults.json`:
```json
{
    "autoconnect": true,
    "reconnect": true,
    "shared": false,
    "quality": 8
}
```

## MIME Types Supported

The HTTP server currently supports these MIME types for noVNC:

- `.html` → `text/html`
- `.css` → `text/css`
- `.js` → `application/javascript`
- `.json` → `application/json`
- `.png` → `image/png`
- `.jpg`, `.jpeg` → `image/jpeg`
- `.gif` → `image/gif`
- `.svg` → `image/svg+xml`
- `.ico` → `image/x-icon`
- `.wasm` → `application/wasm`
- `.ttf` → `font/ttf`
- `.woff` → `font/woff`
- `.woff2` → `font/woff2`
- `.xml` → `application/xml`
- `.txt` → `text/plain`

## Architecture

```
Browser
   |
   | HTTP GET /vnc.html
   v
ws2socket (port 6080)
   |--- Serves static files from web_root
   |
   | WebSocket Upgrade
   v
ws2socket
   |
   | Bidirectional Proxy
   |
   v
VNC Server (port 5901)
```

## Testing

### Test Static File Serving

```bash
curl -v http://localhost:6080/vnc.html
```

Should return the noVNC HTML file.

### Test WebSocket Upgrade

```bash
# Using websocat
websocat ws://localhost:6080/websockify
```

Should successfully establish WebSocket connection and proxy to VNC.

### Monitor Logs

Run with `--verbose` to see:
- HTTP requests received
- WebSocket handshakes
- Proxy connections
- Data transfer statistics

## Production Deployment

### Systemd Service

Create `/etc/systemd/system/ws2socket.service`:

```ini
[Unit]
Description=WebSocket to TCP Socket Proxy
After=network.target

[Service]
Type=simple
User=vnc
Group=vnc
ExecStart=/usr/local/bin/ws2socket \
  --listen 0.0.0.0:6080 \
  --target 127.0.0.1:5901 \
  --web-root /opt/novnc \
  --log-file /var/log/ws2socket.log \
  --daemon \
  --pid-file /var/run/ws2socket.pid
Restart=on-failure
RestartSec=5s

[Install]
WantedBy=multi-user.target
```

Enable and start:

```bash
sudo systemctl enable ws2socket
sudo systemctl start ws2socket
sudo systemctl status ws2socket
```

### Security Considerations

1. **No SSL/TLS for HTTP**: Current build doesn't have HTTPS implemented.

2. **Nginx Reverse Proxy** (recommended for production):

```nginx
server {
    listen 443 ssl http2;
    server_name vnc.example.com;
    
    ssl_certificate /etc/ssl/certs/vnc.example.com.crt;
    ssl_certificate_key /etc/ssl/private/vnc.example.com.key;
    
    location / {
        proxy_pass http://localhost:6080;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
    }
}
```

3. **Firewall**: Only expose ws2socket to localhost, let nginx/Apache handle external connections.

## Future Enhancements

### High Priority
- [ ] Native SSL/TLS support (implement `server_init()` SSL context)

### Medium Priority
- [ ] Test token-based authentication

### Low Priority
- [ ] Connection limits and rate limiting
- [ ] Health check endpoint

## Troubleshooting

### Issue: "Failed to parse target server"

Ensure target is in format `host:port`:
```bash
--target 127.0.0.1:5901
```

### Issue: "404 Not Found" for static files

Check web_root is set and contains noVNC files:
```bash
ls /path/to/novnc/vnc.html
```

### Issue: WebSocket connection fails

1. Check VNC server is running: `netstat -tlnp | grep 5901`
2. Check ws2socket can connect: `telnet 127.0.0.1 5901`
3. Check WebSocket handshake in browser dev tools (Network tab)

### Issue: "Connection refused" to VNC

VNC server not running or wrong port. Check with:
```bash
ps aux | grep vnc
netstat -tlnp | grep vnc
```

## Performance Notes

- **Buffer size**: Default 16KB, configurable via `proxy.buffer_size`
- **Connection timeout**: Default 60s, configurable via `server.socket_timeout`
- **select() based**: Handles ~1000 concurrent connections efficiently
- **No threading**: Single process, event-driven architecture

For high concurrency (>1000 connections), we consider implementing epoll/kqueue support.

## References

- [noVNC GitHub](https://github.com/novnc/noVNC)
- [RFC 6455 - WebSocket Protocol](https://tools.ietf.org/html/rfc6455)
- [websockify](https://github.com/novnc/websockify) - Original Python implementation
