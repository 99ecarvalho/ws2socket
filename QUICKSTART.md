# Quick Start

This guide takes you from a fresh checkout to a working browser VNC session in
about five minutes. It assumes a Debian or Ubuntu system; package names differ
slightly on other distributions.

For background and the full option list, see the [README](README.md) and
`man ws2socket`.

## 1. Install build dependencies

```bash
sudo apt-get install build-essential cmake libssl-dev zlib1g-dev
```

Doxygen is optional. Install it (`sudo apt-get install doxygen`) if you also
want the HTML API reference.

## 2. Build

From the root of the source tree:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Check the result:

```bash
./build/ws2socket --version
```

You should see `ws2socket version 0.1.0` followed by the copyright notice.

Installing is optional. To put `ws2socket` and its man page under
`/usr/local`, run:

```bash
sudo cmake --install build
```

## 3. Try it with a simple TCP service

You don't need VNC to see the proxy work. In one terminal, start a TCP echo
service on port 5900:

```bash
ncat -l -k 127.0.0.1 5900 --exec /bin/cat    # from the "ncat" package
```

In a second terminal, start ws2socket and point it at that service:

```bash
./build/ws2socket --listen 127.0.0.1:6080 --target 127.0.0.1:5900 --verbose
```

In a third terminal, connect with the interactive client that ships with the
Python [websockets](https://pypi.org/project/websockets/) package
(`sudo apt-get install python3-websockets` or `pip install websockets`):

```bash
python3 -m websockets ws://127.0.0.1:6080/
```

Type `hello` and press Enter. The text travels over the WebSocket to
ws2socket, then over TCP to the echo service, and comes back. ws2socket always
replies with binary frames, so the client prints the echo in hex:

```text
Connected to ws://127.0.0.1:6080/.
> hello
< (binary) 68656c6c6f
```

The ws2socket terminal logs the connection and, when it closes, the traffic
statistics.

> [!NOTE]
> ws2socket currently requires clients to offer `permessage-deflate`
> compression. Browsers and the Python `websockets` client do this by default.
> Some command-line tools, such as websocat, do not, and will reject the
> handshake.

## 4. Use it with noVNC

### Get noVNC

```bash
git clone --depth 1 https://github.com/novnc/noVNC.git ~/noVNC
```

### Start a VNC server

Any VNC server works. Two common choices:

```bash
# Share your existing X display on port 5900
x11vnc -display :0 -localhost -forever -shared -nopw

# Or start a new virtual desktop on display :1 (port 5901)
Xvnc :1 -geometry 1280x720 -depth 24 -localhost
```

> [!WARNING]
> `-nopw` disables the VNC password. Only use it on a machine you trust, and
> keep `-localhost` so the VNC server itself is not exposed.

### Start ws2socket

Point `--target` at your VNC server's port and `--web-root` at the noVNC
directory:

```bash
./build/ws2socket \
    --listen 0.0.0.0:6080 \
    --target 127.0.0.1:5900 \
    --web-root ~/noVNC
```

### Connect

Open this URL in a browser:

```text
http://<server-address>:6080/vnc.html
```

Click **Connect**. noVNC connects back to the same host and port, and
ws2socket forwards the session to your VNC server. To connect automatically,
append `?autoconnect=1` to the URL.

## 5. Use a configuration file (optional)

Instead of passing options every time, copy the example configuration and
edit it:

```bash
sudo cp ws2socket.conf.example /etc/ws2socket.conf
sudoedit /etc/ws2socket.conf           # set target, web_root, logging...
ws2socket --config /etc/ws2socket.conf
```

> [!NOTE]
> Values in the configuration file override command-line options.

## Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| `Invalid configuration` at startup | No target was given. Add `--target host:port` or `target =` under `[proxy]`. |
| `Failed to start listening` | The port is already in use. Find the owner with `ss -ltnp 'sport = :6080'`, or choose another port with `--listen`. |
| Browser gets `426 Upgrade Required` | No `--web-root` was set, so plain HTTP requests are refused. |
| `404` for `vnc.html` | `--web-root` does not point at the noVNC directory. Check that `<web-root>/vnc.html` exists. |
| `Failed to connect to target` in the log | The target service is not listening. Check it with `ss -ltn` or `nc -vz 127.0.0.1 5900`. |
| `wss://` or HTTPS connections fail | Native TLS is not finished yet. Put nginx or another TLS proxy in front, as described in the [noVNC guide](docs/NOVNC_GUIDE.md#running-behind-nginx-tls). |

Run with `--verbose` to log each HTTP request, the WebSocket handshake and
per-connection traffic statistics.

## Next steps

- [docs/NOVNC_GUIDE.md](docs/NOVNC_GUIDE.md): running as a systemd service,
  TLS with nginx, and noVNC URL options.
- [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md): how ws2socket works
  internally, known limitations, and a Yocto recipe.
