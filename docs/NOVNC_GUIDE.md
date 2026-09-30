# Using ws2socket with noVNC

[noVNC](https://github.com/novnc/noVNC) is a VNC client that runs in the
browser. Browsers cannot open raw TCP connections, so noVNC talks WebSocket and
needs a proxy to reach the VNC server. ws2socket is that proxy, and it can
serve the noVNC files as well, so one process does both jobs.

```text
Browser ──HTTP GET /vnc.html──▶ ws2socket ── serves files from --web-root
Browser ◀──── WebSocket ─────▶ ws2socket ◀──── TCP ────▶ VNC server :5900
```

If you have not built ws2socket yet, start with the
[Quick Start](../QUICKSTART.md).

## Contents

- [Basic setup](#basic-setup)
- [How a session works](#how-a-session-works)
- [noVNC URL parameters](#novnc-url-parameters)
- [Running as a systemd service](#running-as-a-systemd-service)
- [Running behind nginx (TLS)](#running-behind-nginx-tls)
- [Troubleshooting](#troubleshooting)

## Basic setup

### 1. Get noVNC

Use a distribution package (installed as `/usr/share/novnc` on Debian and
Ubuntu) or clone the upstream repository:

```bash
git clone --depth 1 https://github.com/novnc/noVNC.git /opt/novnc
```

### 2. Start a VNC server

Any RFB-compatible server works. Keep it bound to localhost so it is only
reachable through ws2socket.

With **x11vnc**, sharing an existing X display (port 5900):

```bash
# Create a VNC password once
mkdir -p ~/.vnc
x11vnc -storepasswd ~/.vnc/passwd
chmod 600 ~/.vnc/passwd

x11vnc -display :0 -localhost -forever -shared -rfbauth ~/.vnc/passwd
```

With **Xvnc** (TigerVNC), starting a virtual desktop on display `:1`
(port 5901):

```bash
Xvnc :1 -geometry 1280x720 -depth 24 -localhost -SecurityTypes VncAuth \
     -PasswordFile ~/.vnc/passwd
```

### 3. Start ws2socket

```bash
ws2socket \
    --listen 0.0.0.0:6080 \
    --target 127.0.0.1:5900 \
    --web-root /opt/novnc
```

Add `--verbose` while you are setting things up.

### 4. Open noVNC

Browse to:

```text
http://<server-address>:6080/vnc.html
```

By default noVNC connects to the host and port that served the page, on the
path `websockify`. ws2socket accepts WebSocket upgrades on any path, so no
extra settings are needed.

## How a session works

1. The browser requests `/vnc.html` and its scripts. ws2socket serves them from
   `--web-root`.
2. noVNC sends a WebSocket upgrade request to `/websockify`. ws2socket
   completes the RFC 6455 handshake and forks a child process for the session.
3. The child opens a TCP connection to `--target`.
4. From then on, WebSocket frames from the browser are unwrapped and written to
   the VNC server, and data from the VNC server is sent back as binary
   WebSocket frames.
5. When either side closes, the child logs traffic statistics and exits.

Each session runs in its own process. Several browsers can connect at once, as
long as the VNC server allows shared sessions (for example `-shared` for
x11vnc).

### Served file types

The built-in HTTP server sends these content types:

| Extension | Content-Type |
|---|---|
| `.html`, `.htm` | `text/html` |
| `.css` | `text/css` |
| `.js` | `application/javascript` |
| `.json` | `application/json` |
| `.png` | `image/png` |
| `.jpg`, `.jpeg` | `image/jpeg` |
| `.gif` | `image/gif` |
| `.svg` | `image/svg+xml` |
| `.ico` | `image/x-icon` |
| `.wasm` | `application/wasm` |
| `.txt` | `text/plain` |

Anything else, such as noVNC's `.mp3` and `.oga` bell sounds, is sent as
`application/octet-stream`. That is enough for noVNC to work. A request for a
directory (a path ending in `/`) serves its `index.html`, and any path
containing `..` is rejected.

## noVNC URL parameters

noVNC reads its settings from the query string or the fragment (`#`) of the
page URL. The fragment is not sent to the server, which makes it the better
place for a password. The parameters most useful with ws2socket are:

| Parameter | Meaning |
|---|---|
| `autoconnect` | `true` to connect as soon as the page loads |
| `path` | WebSocket path, relative to the page (default `websockify`) |
| `host`, `port` | Override the WebSocket host and port (default: those of the page) |
| `encrypt` | `true` to use `wss://`. Needed when noVNC is loaded over HTTPS. |
| `password` | VNC password. Prefer the fragment form. |
| `shared` | Ask the VNC server for a shared session (default `true`) |
| `view_only` | `true` to disable keyboard and mouse input |
| `resize` | `off`, `scale` or `remote` |
| `quality`, `compression` | JPEG quality and compression level, `0` to `9` |
| `reconnect`, `reconnect_delay` | Reconnect automatically, and the delay in ms |
| `logging` | Browser console log level: `error`, `warn`, `info` or `debug` |

Examples:

```text
http://vnc.example.com:6080/vnc.html?autoconnect=true&resize=scale
http://vnc.example.com:6080/vnc.html#autoconnect=true&password=secret
```

The full list is in noVNC's
[embedding documentation](https://github.com/novnc/noVNC/blob/master/docs/EMBEDDING.md).
Site-wide defaults can also be set in `defaults.json` and `mandatory.json` in
the noVNC directory.

## Running as a systemd service

systemd handles backgrounding itself, so run ws2socket in the foreground
without `--daemon`. Save the following as
`/etc/systemd/system/ws2socket.service`:

```ini
[Unit]
Description=ws2socket WebSocket to TCP proxy
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/ws2socket --config /etc/ws2socket.conf
Restart=on-failure
RestartSec=5s

# Run unprivileged and restrict what the process can touch
DynamicUser=yes
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=read-only
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
```

A matching `/etc/ws2socket.conf` looks like this:

```ini
[general]
daemon = false

[server]
listen   = 127.0.0.1:6080
web_root = /opt/novnc

[proxy]
target = 127.0.0.1:5900

[logging]
level   = info
console = true
```

Logs go to the journal (`journalctl -u ws2socket`). If you prefer a log file,
set `file =` under `[logging]` and add its directory to `ReadWritePaths=`.

Enable and start the service:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now ws2socket
systemctl status ws2socket
```

## Running behind nginx (TLS)

ws2socket does not authenticate clients, and its native TLS support is not
finished yet (see [Known limitations](IMPLEMENTATION.md#known-limitations)).
For anything reachable from an untrusted network, bind ws2socket to localhost
and let a reverse proxy handle TLS and access control:

```nginx
server {
    listen 443 ssl;
    http2 on;
    server_name vnc.example.com;

    ssl_certificate     /etc/ssl/certs/vnc.example.com.crt;
    ssl_certificate_key /etc/ssl/private/vnc.example.com.key;

    # Optional: require a login before anyone can reach VNC
    # auth_basic           "VNC";
    # auth_basic_user_file /etc/nginx/vnc.htpasswd;

    location / {
        proxy_pass http://127.0.0.1:6080;
        proxy_http_version 1.1;
        proxy_set_header Upgrade    $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host       $host;

        # Keep idle VNC sessions open
        proxy_read_timeout  1h;
        proxy_send_timeout  1h;
    }
}
```

Then open `https://vnc.example.com/vnc.html`. noVNC detects the HTTPS page and
uses `wss://` automatically.

On nginx older than 1.25.1, replace `listen 443 ssl;` and `http2 on;` with
`listen 443 ssl http2;`.

## Troubleshooting

**The browser shows `426 Upgrade Required`.**
ws2socket was started without `--web-root`, so it only accepts WebSocket
connections.

**`404 Not Found` for `vnc.html`.**
`--web-root` must point at the directory that contains `vnc.html`. Check with
`ls <web-root>/vnc.html`.

**noVNC shows "Failed to connect to server".**

1. Check that the VNC server is listening: `ss -ltn | grep 590`.
2. Check that the target is reachable from the ws2socket host:
   `nc -vz 127.0.0.1 5900`.
3. Restart ws2socket with `--verbose` and look for
   `Failed to connect to target`.
4. Open the browser developer tools (Network tab, WS filter) and check the
   handshake. A successful handshake returns status `101`.

**The connection drops after about a minute of inactivity behind a proxy.**
Raise the proxy's read timeout, for example `proxy_read_timeout` in nginx as
shown above.

**Changing `--target` has no effect.**
If you also pass `--config`, the `target` value in the file takes precedence
over the command line.

## References

- [noVNC](https://github.com/novnc/noVNC)
- [websockify](https://github.com/novnc/websockify), the original Python proxy
- [RFC 6455: The WebSocket Protocol](https://datatracker.ietf.org/doc/html/rfc6455)
