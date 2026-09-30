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
- [Serving over TLS](#serving-over-tls)
- [Several VNC servers with tokens](#several-vnc-servers-with-tokens)
- [Requiring a password](#requiring-a-password)
- [Monitoring with Prometheus](#monitoring-with-prometheus)
- [Running as a systemd service](#running-as-a-systemd-service)
- [Running behind nginx](#running-behind-nginx)
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

## Serving over TLS

Give ws2socket a certificate and key in PEM format, and it serves both the
noVNC page (`https://`) and the WebSocket (`wss://`) over TLS:

```bash
ws2socket \
    --listen 0.0.0.0:6080 \
    --target 127.0.0.1:5900 \
    --web-root /opt/novnc \
    --cert /etc/ssl/certs/vnc.example.com.pem \
    --key /etc/ssl/private/vnc.example.com.key
```

Open `https://vnc.example.com:6080/vnc.html`. noVNC sees that the page was
loaded over HTTPS and uses `wss://` automatically. With TLS enabled, plain
`http://` and `ws://` connections are refused.

The certificate file may contain the full chain (server certificate first).
TLS 1.2 is the minimum version accepted. The key must be readable by the user
ws2socket runs as.

## Several VNC servers with tokens

One ws2socket instance can front many VNC servers. Instead of `--target`, give
it a token file that maps a token to each server. It uses the same format as
websockify's `TokenFile` plugin:

```text
# /etc/ws2socket/tokens
desktop1: 192.168.1.10:5900
desktop2: 192.168.1.11:5900
lab-pc:   lab-pc.internal:5901
```

```bash
ws2socket --token-file /etc/ws2socket/tokens --web-root /opt/novnc
```

Clients choose a server by passing the token in the WebSocket URL. In noVNC,
use the `path` parameter:

```text
http://vnc.example.com:6080/vnc.html?path=websockify%3Ftoken%3Ddesktop1
```

- The token is read from the `token` query parameter, or else from the first
  path component (`/desktop1`).
- Requests with a missing or unknown token get `403 Forbidden`.
- `--token-file` can also name a directory, in which case every file in it is
  read. Edits are picked up automatically within about a minute, without a
  restart.
- Tokens decide where a client connects, so treat them like passwords: make
  them long and random (for example `openssl rand -hex 16`), and use TLS so
  they are not sent in the clear.

## Requiring a password

`--auth-file` makes ws2socket require HTTP Basic authentication for every
request: the noVNC page, its WebSocket, and `/metrics`. The browser asks once
and normally reuses the credentials for the WebSocket connection too.

Create the password file with `htpasswd` (from `apache2-utils`) or with
`openssl`:

```bash
htpasswd -B -c /etc/ws2socket/htpasswd alice                    # bcrypt
printf 'bob:%s\n' "$(openssl passwd -6)" >> /etc/ws2socket/htpasswd  # SHA-512-crypt
chmod 600 /etc/ws2socket/htpasswd
```

```bash
ws2socket --cert /etc/ssl/certs/vnc.pem --key /etc/ssl/private/vnc.key \
          --auth-file /etc/ws2socket/htpasswd \
          --target 127.0.0.1:5900 --web-root /opt/novnc
```

- Always combine it with TLS, because Basic authentication sends the password
  with every request.
- Supported hashes are bcrypt (`$2y$`/`$2b$`), SHA-256/512-crypt (`$5$`/`$6$`)
  and yescrypt (`$y$`). `htpasswd`'s default MD5 format (`$apr1$`) is **not**
  supported, so pass `-B`.
- Changes to the file take effect without a restart. Failed logins are delayed
  by one second and counted in the metrics.
- The realm shown by the browser can be set with `auth_realm` under
  `[server]`.
- Passwords and tokens can be combined: users log in, then the token picks the
  VNC server.

## Monitoring with Prometheus

`--metrics` (or `metrics = true` under `[server]`) serves counters at
`/metrics` in the Prometheus text format:

```text
ws2socket_connections_active 3
ws2socket_websocket_sessions_active 2
ws2socket_bytes_total{direction="to_client"} 48213377
ws2socket_rejected_total{reason="auth"} 4
ws2socket_http_responses_total{code="200"} 118
```

A Prometheus scrape job, using the same credentials as the users:

```yaml
scrape_configs:
  - job_name: ws2socket
    scheme: https
    basic_auth:
      username: monitor
      password_file: /etc/prometheus/ws2socket.pass
    static_configs:
      - targets: ["vnc.example.com:6080"]
```

Without `--auth-file`, anyone who can reach the port can read `/metrics`, so
restrict it with a firewall in that case.

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

With `DynamicUser=yes`, the service cannot read files that only root may read,
such as a TLS private key or a `chmod 600` password file. Hand them over with
`LoadCredential=`, and refer to them through `%d`, the credentials
directory:

```ini
[Service]
LoadCredential=tls.key:/etc/ssl/private/vnc.example.com.key
LoadCredential=htpasswd:/etc/ws2socket/htpasswd
ExecStart=
ExecStart=/usr/local/bin/ws2socket --config /etc/ws2socket.conf \
          --key %d/tls.key --auth-file %d/htpasswd
```

systemd copies credentials when the service starts, so restart the service
after editing the password file.

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

## Running behind nginx

ws2socket can handle TLS and passwords itself. A reverse proxy is still
useful to share port 443 with other sites, or for login methods that
ws2socket does not provide, such as single sign-on. In that case, bind
ws2socket to localhost and let the proxy handle TLS and access control:

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
   `Failed to connect to target`. The browser sees this as `502 Bad Gateway`.
4. Open the browser developer tools (Network tab, WS filter) and check the
   handshake. A successful handshake returns status `101`.

**The connection drops after about a minute of inactivity behind a proxy.**
Raise the proxy's read timeout, for example `proxy_read_timeout` in nginx as
shown above.

**The browser keeps asking for the password.**
Check the log for `Authentication failed`, and for startup warnings about the
password file, such as an unsupported hash (`$apr1$` needs `htpasswd -B`
instead).

**The client gets `403 Forbidden`.**
A token file is in use, and the WebSocket URL has no token or an unknown one.
Check the `path` parameter in the noVNC URL.

**The client gets `502 Bad Gateway`.**
ws2socket could not connect to the target. See the previous item.

**The client gets `503 Service Unavailable`.**
`max_connections` clients are already connected. Raise the limit under
`[server]` if needed.

## References

- [noVNC](https://github.com/novnc/noVNC)
- [websockify](https://github.com/novnc/websockify), the original Python proxy
- [RFC 6455: The WebSocket Protocol](https://datatracker.ietf.org/doc/html/rfc6455)
