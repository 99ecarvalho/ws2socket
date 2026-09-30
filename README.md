# ws2socket

[![License: LGPL v3+](https://img.shields.io/badge/license-LGPL--3.0--or--later-blue.svg)](COPYING.LESSER)
![Language: C11](https://img.shields.io/badge/language-C11-555.svg)
![Platform: Linux](https://img.shields.io/badge/platform-Linux-lightgrey.svg)
![Status: alpha](https://img.shields.io/badge/status-alpha-orange.svg)

**A small, dependency-light WebSocket-to-TCP proxy written in C.**

ws2socket lets a browser talk to any TCP service through a WebSocket. It
accepts WebSocket connections, forwards the traffic unchanged to a TCP server,
and can serve static files, so a single binary can host the
[noVNC](https://github.com/novnc/noVNC) web client *and* bridge it to your VNC
server.

It does the same job as [websockify](https://github.com/novnc/websockify), but
it is a single ~90 KB native binary that needs only libc, pthreads, OpenSSL and
zlib. That makes it a good fit for embedded Linux and Yocto images where
shipping a Python runtime is not an option.

```text
 Browser (noVNC)                ws2socket                  TCP service
┌──────────────┐  HTTP GET   ┌──────────────┐           ┌──────────────┐
│              │────────────▶│ static files │           │              │
│              │  WebSocket  │              │    TCP    │  VNC server  │
│              │◀───────────▶│    proxy     │◀─────────▶│  :5900       │
└──────────────┘   :6080     └──────────────┘           └──────────────┘
```

## Contents

- [Features](#features)
- [Project status](#project-status)
- [Quick start](#quick-start)
- [Building](#building)
- [Usage](#usage)
- [Configuration](#configuration)
- [Deployment notes](#deployment-notes)
- [Documentation](#documentation)
- [Contributing](#contributing)
- [Security](#security)
- [License](#license)

## Features

- **RFC 6455 WebSocket server**: handshake, all three payload-length
  encodings, unmasking, fragmented messages, ping/pong and close handling.
- **Transparent TCP bridging**: bytes are forwarded as-is in both directions.
- **Native TLS**: serve `https://` and `wss://` directly with a PEM
  certificate and key.
- **Token-based routing**: one instance can front many VNC servers, using
  websockify-compatible token files (`?token=...`).
- **permessage-deflate** (RFC 7692) compression, negotiated when the client
  offers it.
- **Built-in static file server** with the MIME types noVNC needs, and
  path-traversal protection.
- **Process-per-connection model**: a misbehaving client cannot take down
  other sessions. The number of simultaneous clients is capped by
  `max_connections`.
- **INI configuration file** in addition to command-line options.
- **Logging** to the console, a file and/or syslog, with five levels.
- **Daemon mode** with a PID file.
- **Small footprint**: C11 with no runtime dependencies beyond OpenSSL, zlib
  and the C library.
- **Yocto-friendly**: plain CMake build with an install target and a man page.

## Project status

ws2socket is **alpha** software (version 0.1.0). The core features work and
are covered by an end-to-end test suite, but the project is young, and its
interfaces may still change before 1.0.

Known limitations:

- **No user authentication.** Tokens select a target, but anyone who knows a
  token can use it. For untrusted networks, add authentication in a reverse
  proxy. See [SECURITY.md](SECURITY.md).
- **Minimal HTTP server.** There is no keep-alive, `HEAD` or range request
  support, and no URL decoding of file names. It is enough for noVNC.
- **Metrics** are collected internally but not yet exported.

See [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md#known-limitations) for
details.

## Quick start

```bash
# Dependencies (Debian/Ubuntu)
sudo apt-get install build-essential cmake libssl-dev zlib1g-dev

# Get the source and build
git clone https://github.com/99ecarvalho/ws2socket.git
cd ws2socket
cmake -S . -B build
cmake --build build

# Proxy WebSocket clients on port 6080 to a VNC server on port 5900
./build/ws2socket --listen 0.0.0.0:6080 --target 127.0.0.1:5900
```

For a step-by-step walkthrough, including serving noVNC, see
**[QUICKSTART.md](QUICKSTART.md)**.

## Building

### Requirements

| Dependency | Version | Debian/Ubuntu package |
|---|---|---|
| C compiler (GCC or Clang) | C11 | `build-essential` |
| CMake | 3.10 or newer | `cmake` |
| OpenSSL | 1.1.1 or newer (3.x recommended) | `libssl-dev` |
| zlib | any | `zlib1g-dev` |
| Doxygen (optional, for API docs) | any | `doxygen` |

ws2socket targets Linux. Other POSIX systems may work but are untested.

### Compile

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The binary is written to `build/ws2socket`. If Doxygen is installed, the API
reference is generated in `docs/html/` as part of the default build. You can
also run it on its own with `cmake --build build --target docs`.

### Install

```bash
sudo cmake --install build            # installs under /usr/local by default
sudo cmake --install build --prefix /usr
```

This installs `bin/ws2socket` and the man page `share/man/man1/ws2socket.1`.

### Run the tests

The integration tests start the built binary and exercise it with real
WebSocket clients. They need Python 3 with the
[websockets](https://pypi.org/project/websockets/) package, plus the
`openssl` command for the TLS tests:

```bash
sudo apt-get install python3-websockets openssl
ctest --test-dir build --output-on-failure
# or, with per-test output:
python3 tests/test_integration.py build/ws2socket
```

### Docker

The [docker/](docker/) directory builds a small runtime image (about 80 MB,
running as a non-root user):

```bash
docker build -f docker/Dockerfile -t ws2socket .
docker run --rm -p 6080:6080 ws2socket --target 192.168.1.50:5900
```

See [docker/README.md](docker/README.md) for noVNC, configuration files and
reaching services on the Docker host.

## Usage

```text
ws2socket [OPTIONS]

  -h, --help                Print this help message
      --version             Print version information
  -l, --listen HOST[:PORT]  Listen address or port (default: 0.0.0.0:6080)
  -p, --port PORT           Listen port (default: 6080)
  -t, --target HOST:PORT    Target TCP server
      --token-file FILE     Choose the target per client from a token file
  -w, --web-root DIR        Serve static files from DIR (e.g. noVNC)
  -f, --config FILE         Read settings from an INI file
  -c, --cert FILE           TLS certificate (PEM), enables https:// and wss://
  -k, --key FILE            TLS private key (PEM)
  -v, --verbose             Debug logging
      --log-file FILE       Also log to FILE
      --daemon              Run in the background
      --pid-file FILE       Write the daemon PID to FILE
```

### Examples

Serve noVNC and proxy to a local VNC server:

```bash
ws2socket --listen 0.0.0.0:6080 --target 127.0.0.1:5900 \
          --web-root /usr/share/novnc
# then open http://<host>:6080/vnc.html
```

Serve over TLS (`https://` and `wss://`):

```bash
ws2socket --cert /etc/ssl/certs/vnc.pem --key /etc/ssl/private/vnc.key \
          --target 127.0.0.1:5900 --web-root /usr/share/novnc
```

Front several VNC servers with one instance, using a token file:

```bash
cat > /etc/ws2socket/tokens <<'TOKENS'
desktop1: 192.168.1.10:5900
desktop2: 192.168.1.11:5900
TOKENS
ws2socket --token-file /etc/ws2socket/tokens --web-root /usr/share/novnc
# then open http://<host>:6080/vnc.html?path=websockify%3Ftoken%3Ddesktop1
```

Run as a daemon with a config file:

```bash
ws2socket --config /etc/ws2socket.conf --daemon --pid-file /run/ws2socket.pid
```

Troubleshoot a connection with debug logging:

```bash
ws2socket --target 127.0.0.1:5900 --verbose --log-file /tmp/ws2socket.log
```

Any HTTP request that is not a WebSocket upgrade is answered from `--web-root`,
or with `426 Upgrade Required` if no web root is set. WebSocket upgrades are
accepted on any path, so noVNC's default `/websockify` path works unchanged.
A client whose target cannot be reached gets `502 Bad Gateway`.

## Configuration

Settings can also be read from an INI-style file passed with `--config`. An
annotated example is included as
[ws2socket.conf.example](ws2socket.conf.example):

```ini
[general]
daemon   = false
pid_file = /var/run/ws2socket.pid

[server]
listen   = 0.0.0.0:6080
web_root = /usr/share/novnc

[proxy]
target = 127.0.0.1:5900

[logging]
level   = info            ; debug, info, warning, error, critical
file    = /var/log/ws2socket.log
syslog  = false
```

Command-line options override values from the file, so a shared config can be
adjusted per run (for example `--config site.conf --verbose`). Invalid values
are reported with their file name and line number. The full list of keys is in
the man page (`man ws2socket`).

## Deployment notes

- **Use TLS and restrict access for anything public.** ws2socket can
  terminate TLS itself (`--cert`/`--key`), but it does no user
  authentication. On untrusted networks, put a reverse proxy (nginx, Caddy,
  HAProxy) in front for authentication, or limit access with a firewall. An
  nginx example is in
  [docs/NOVNC_GUIDE.md](docs/NOVNC_GUIDE.md#running-behind-nginx).
- **Under systemd, run in the foreground** (no `--daemon`) with `Type=simple`.
  A sample unit file is in the noVNC guide.
- **Yocto**: the project builds with `inherit cmake`. A sample recipe is in
  [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md#yocto-integration).

## Documentation

| Document | Contents |
|---|---|
| [QUICKSTART.md](QUICKSTART.md) | Build, run and connect in a few minutes |
| [docs/NOVNC_GUIDE.md](docs/NOVNC_GUIDE.md) | Serving noVNC, systemd, nginx/TLS, troubleshooting |
| [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md) | Architecture, source layout, limitations, Yocto recipe |
| [CONTRIBUTING.md](CONTRIBUTING.md) | How to report bugs and submit changes |
| [SECURITY.md](SECURITY.md) | Reporting vulnerabilities, safe deployment |
| [docs/ws2socket.1](docs/ws2socket.1) | Man page (`man ./docs/ws2socket.1`) |
| `docs/html/` | Doxygen API reference (generated at build time) |

## Contributing

Contributions are welcome, from bug reports and documentation fixes to code.
Please read [CONTRIBUTING.md](CONTRIBUTING.md) for the development setup,
coding style and commit conventions. The gaps listed under
[Project status](#project-status), along with a test suite, are the most useful
places to start.

## Security

Please report vulnerabilities privately, as described in
[SECURITY.md](SECURITY.md), and not in public issues. The same file explains
how to deploy ws2socket safely given its current limitations.

## License

Copyright (c) 2026 Eduardo Correia <ecorreia@apliant.com.br>

ws2socket is free software: you can redistribute it and/or modify it under the
terms of the **GNU Lesser General Public License, version 3 or (at your option)
any later version**. The license text is in [COPYING.LESSER](COPYING.LESSER);
it supplements the GNU General Public License v3, included as [COPYING](COPYING).

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.

## Acknowledgements

- [websockify](https://github.com/novnc/websockify) and
  [noVNC](https://github.com/novnc/noVNC), whose design ws2socket follows.
- [RFC 6455](https://datatracker.ietf.org/doc/html/rfc6455) (The WebSocket
  Protocol) and [RFC 7692](https://datatracker.ietf.org/doc/html/rfc7692)
  (Compression Extensions for WebSocket).
