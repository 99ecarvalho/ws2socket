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
- [License](#license)

## Features

- **RFC 6455 WebSocket server**: handshake, all three payload-length
  encodings, unmasking, fragmented messages, ping/pong and close handling.
- **Transparent TCP bridging**: bytes are forwarded as-is in both directions.
- **permessage-deflate** (RFC 7692) compression, negotiated automatically.
- **Built-in static file server** with the MIME types noVNC needs, and
  path-traversal protection.
- **Process-per-connection model**: a misbehaving client cannot take down
  other sessions.
- **INI configuration file** in addition to command-line options.
- **Logging** to the console, a file and/or syslog, with five levels.
- **Daemon mode** with a PID file.
- **Small footprint**: C11 with no runtime dependencies beyond OpenSSL, zlib
  and the C library.
- **Yocto-friendly**: plain CMake build with an install target and a man page.

## Project status

ws2socket is **alpha** software (version 0.1.0). Plain `ws://` proxying and
static file serving work and are used with noVNC. The following are known gaps
you should be aware of before deploying:

| Area | Status |
|---|---|
| `ws://` proxying | Working |
| Static file serving | Working |
| permessage-deflate | Working, but **always** advertised in the handshake. Clients that do not offer compression (browsers and Python `websockets` do; websocat does not) reject the connection. |
| Native TLS (`wss://`) | **Incomplete.** The certificate loads and the TLS handshake runs, but data is not yet sent over the TLS session. Use a TLS-terminating reverse proxy for now. |
| Token-based routing (`token_file`) | **Not implemented.** The option is parsed, but every client goes to the single configured target. |
| Client authentication | None. Anyone who can reach the port can reach the target. |
| Option precedence | Values in the config file override command-line options. |
| Automated tests | Not yet available. |

See [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md#known-limitations) for the
full list.

## Quick start

```bash
# Dependencies (Debian/Ubuntu)
sudo apt-get install build-essential cmake libssl-dev zlib1g-dev

# Build
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
  -l, --listen HOST[:PORT]  Listen address (default: 0.0.0.0:6080)
  -p, --port PORT           Listen port (default: 6080)
  -t, --target HOST:PORT    Target TCP server (required)
  -w, --web-root DIR        Serve static files from DIR (e.g. noVNC)
  -f, --config FILE         Read settings from an INI file
  -c, --cert FILE           TLS certificate (PEM), see "Project status"
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
target      = 127.0.0.1:5900
buffer_size = 65536

[logging]
level   = info            ; debug, info, warning, error, critical
file    = /var/log/ws2socket.log
syslog  = false
```

> [!NOTE]
> The config file is read after the command line, so its values currently take
> precedence over command-line options.

The full list of keys is in the man page (`man ws2socket`).

## Deployment notes

- **Put it behind a reverse proxy for anything public.** ws2socket does no
  client authentication, and native TLS is not finished yet. Let nginx, Caddy
  or HAProxy handle HTTPS and access control, and bind ws2socket to
  `127.0.0.1`. An nginx example is in
  [docs/NOVNC_GUIDE.md](docs/NOVNC_GUIDE.md#running-behind-nginx-tls).
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
| [docs/ws2socket.1](docs/ws2socket.1) | Man page (`man ./docs/ws2socket.1`) |
| `docs/html/` | Doxygen API reference (generated at build time) |

## Contributing

Contributions are welcome, whether bug reports, documentation fixes or code.

1. Open an issue describing the bug or feature before starting larger changes.
2. Keep the existing style: C11, K&R with 4-space indentation, and a Doxygen
   comment on every public function.
3. Make sure the build is warning-free with the project flags
   (`-Wall -Wextra -Wpedantic -Wstrict-prototypes`).
4. Add the standard copyright and SPDX header to new source files:

   ```c
   /**
    * @file example.c
    * @brief One-line description
    * @author Your Name <you@example.com>
    *
    * @copyright Copyright (c) 2026 Your Name <you@example.com>
    *
    * SPDX-License-Identifier: LGPL-3.0-or-later
    */
   ```

The gaps listed under [Project status](#project-status), along with a test
suite, are the most useful places to start.

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
