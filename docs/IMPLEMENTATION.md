# ws2socket Internals

This document describes how ws2socket is put together. It is meant for
contributors and for anyone integrating ws2socket into a larger system. For
using the program, see the [README](../README.md) and the
[Quick Start](../QUICKSTART.md).

The generated Doxygen reference (`docs/html/index.html`, built with
`cmake --build build --target docs`) documents every function and structure.
This document covers the big picture.

## Contents

- [Design goals](#design-goals)
- [Process model](#process-model)
- [Request flow](#request-flow)
- [Source layout](#source-layout)
- [Modules](#modules)
- [Configuration handling](#configuration-handling)
- [Error handling](#error-handling)
- [Known limitations](#known-limitations)
- [Build system](#build-system)
- [Yocto integration](#yocto-integration)
- [Coding conventions](#coding-conventions)
- [Roadmap](#roadmap)

## Design goals

- **Replace websockify where Python is unavailable.** Keep the same model: one
  listening port, static files for the web client, and a WebSocket bridged to a
  TCP target.
- **Few dependencies.** Only libc, pthreads, OpenSSL (for SHA-1 in the
  handshake and for TLS) and zlib (for permessage-deflate).
- **Simple, auditable code.** Plain C11, blocking I/O and `select()`, with no
  event-loop framework.
- **Fault isolation.** A crash or hang in one session must not affect the
  others.

## Process model

ws2socket uses a **process-per-connection** model: the listener forks one child for each accepted client.

```text
                    ┌──────────────────────────┐
                    │ parent: accept() loop    │
                    │ (server_run, server.c)   │
                    └────────────┬─────────────┘
                     fork() per accepted client
            ┌────────────────────┼────────────────────┐
            ▼                    ▼                    ▼
   ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐
   │ child: HTTP     │  │ child: WebSocket│  │ child: WebSocket│
   │ serve one file, │  │ ⇄ TCP select()  │  │ ⇄ TCP select()  │
   │ then exit       │  │ loop            │  │ loop            │
   └─────────────────┘  └─────────────────┘  └─────────────────┘
```

- The parent only accepts connections. If TLS is configured, it also runs the
  TLS handshake. It then forks. A `SIGCHLD` handler reaps finished children.
- Each child handles exactly one HTTP request or one WebSocket session, then
  exits.
- Inside a child, `proxy_forward()` runs a `select()` loop over two
  descriptors: the client socket and the target socket. An idle `select()`
  timeout (`socket_timeout`) does not close the session.

Consequences:

- Sessions are fully isolated, and there is no shared mutable state between
  them.
- Per-session memory is one process. That is cheap with copy-on-write, but a
  very high connection count costs more than it would with an event loop.
- Anything that is meant to span sessions (metrics, connection pooling) would
  need shared memory or IPC, because each child has its own copy. See
  [Known limitations](#known-limitations).

## Request flow

`handle_client()` in `src/ws2socket.c` runs in the child:

1. `server_recv_request()` reads and parses the HTTP request (method, path,
   up to 32 headers).
2. If the request is **not** a WebSocket upgrade
   (`http_is_websocket_upgrade()`):
   - with a web root, `http_serve_file()` sends the file or an error status;
   - without one, the reply is `426 Upgrade Required`.

   The connection is then closed.
3. For an upgrade, `websocket_accept()` computes
   `Sec-WebSocket-Accept = base64(SHA1(key + GUID))`, sends `101 Switching
   Protocols` and sets up permessage-deflate.
4. `proxy_connect_target()` opens the TCP connection to the configured target,
   with a timeout.
5. `proxy_forward()` copies data in both directions until either side closes:
   - WebSocket to TCP: the frame is read, unmasked, reassembled if fragmented
     and decompressed if needed, and the payload is written to the target.
     Ping frames get a pong; a close frame ends the loop.
   - TCP to WebSocket: whatever is read from the target is sent as one binary
     frame.
6. On exit, per-session byte counts, including the compression ratio, are
   logged.

## Source layout

```text
.
├── CMakeLists.txt           Build definition
├── Doxyfile.in              Doxygen template (configured by CMake)
├── ws2socket.conf.example   Annotated configuration file
├── include/                 Public headers, one per module
├── src/                     Implementation
├── docs/
│   ├── ws2socket.1          Man page
│   ├── IMPLEMENTATION.md    This document
│   ├── NOVNC_GUIDE.md       noVNC deployment guide
│   └── html/                Generated API reference (not in git)
└── docker/                  Container build helper
```

## Modules

| Module | Files | Responsibility |
|---|---|---|
| Application | `src/ws2socket.c` | `main()`, signal handling, daemonization, per-client handler |
| Configuration | `include/config.h`, `src/config.c` | Defaults, command-line parsing, INI file loading, validation, `--help` and `--version` |
| Server | `include/server.h`, `src/server.c` | Listening socket, TLS context, `accept()` and fork loop, HTTP request reading |
| HTTP | `src/http_server.c` | Upgrade detection, header lookup, static files, MIME types |
| WebSocket | `include/websocket.h`, `src/websocket.c`, `src/websocket_impl.c` | RFC 6455 handshake, frame codec, masking, fragmentation, control frames, permessage-deflate |
| Proxy | `include/proxy.h`, `src/proxy.c` | Target connection, bidirectional `select()` forwarding, statistics |
| Utilities | `include/utils.h`, `src/utils.c` | Safe strings, `host:port` parsing, Base64, SHA-1, random bytes, socket helpers, circular buffer |
| Logging | `include/logging.h`, `src/logging.c` | Levels, and console, file and syslog targets (mutex-protected) |
| Token auth | `include/token_auth.h`, `src/token_auth.c` | Token-file parser and hash-table lookup (not wired in yet) |
| Connection pool | `include/conn_pool.h`, `src/conn_pool.c` | LRU pool of target connections (see limitations) |
| Metrics | `include/metrics.h`, `src/metrics.c` | Counters with Prometheus and JSON export (not wired in yet) |
| Common | `include/common.h` | Error codes, limits, WebSocket opcodes and states |

### WebSocket details

- Supports all three payload-length encodings (7-bit, 16-bit and 64-bit).
- Unmasks client frames. Server frames are sent unmasked, as RFC 6455
  requires.
- Reassembles continuation frames into one message before delivery.
- permessage-deflate uses raw DEFLATE (`windowBits = -15`) with
  `server_no_context_takeover` and `client_no_context_takeover`, so no state
  is kept between messages.
- Data from the target is always sent as **binary** frames, which is what
  noVNC and websockify clients expect.

### HTTP details

- Requests are read until the end of the headers. Only `GET` is meaningful.
- The request path is joined to the web root. A trailing `/` maps to
  `index.html`. Any path containing `..` is rejected.
- MIME types: `html`/`htm`, `css`, `js`, `json`, `png`, `jpg`/`jpeg`, `gif`,
  `svg`, `ico`, `wasm` and `txt`. Everything else is sent as
  `application/octet-stream`.
- Every response is `Connection: close`, so there is no keep-alive.

### Logging

| Level | Config value | Use |
|---|---|---|
| `LOG_DEBUG` | `debug` | Frames, headers, per-read detail (`--verbose`) |
| `LOG_INFO` | `info` | Connections, configuration summary (the default) |
| `LOG_WARN` | `warning` | Recoverable oddities, such as a bad config line |
| `LOG_ERROR` | `error` | Failed operations |
| `LOG_CRITICAL` | `critical` | Startup failures |

The targets `LOG_TARGET_CONSOLE` (stderr), `LOG_TARGET_FILE` and
`LOG_TARGET_SYSLOG` can be combined. The syslog facility is `LOG_LOCAL0`.

## Configuration handling

`main()` builds the configuration in this order:

1. `config_init_defaults()`: listen `0.0.0.0:6080`, buffer 64 KiB, up to 1024
   connections, 60 s socket timeout, log level `info` to the console.
2. `config_parse_args()`: command-line options.
3. `config_load_file()`: the INI file, if `--config` was given.
4. `config_validate()`: requires a non-zero port, a target, and both a
   certificate and a key if either is set.

Because step 3 runs after step 2, **file values override command-line
values**. This is the reverse of the usual convention and is listed as a
limitation below.

The recognized sections and keys are:

| Section | Keys |
|---|---|
| `[general]` | `daemon`, `pid_file`, `token_file` |
| `[server]` | `listen`, `port`, `cert_file`, `key_file`, `max_connections`, `socket_timeout`, `web_root` |
| `[proxy]` | `target`, `buffer_size`, `max_connections`, `socket_timeout` |
| `[logging]` | `level`, `file`, `console`, `syslog` |

Unknown keys are ignored silently.

## Error handling

Functions return `int` status codes from `common.h`, or `NULL` or `-1` for
constructors and I/O:

| Code | Value | Meaning |
|---|---|---|
| `WS_SUCCESS` | 0 | Success |
| `WS_ERROR` | -1 | Generic failure |
| `WS_ENOMEM` | -2 | Out of memory |
| `WS_EINVAL` | -3 | Invalid argument |
| `WS_ECONNREF` | -4 | Connection refused |
| `WS_ESOCKET` | -5 | Socket error |
| `WS_ESSL` | -6 | TLS error |
| `WS_EPROTO` | -7 | Protocol violation |
| `WS_EAUTH` | -8 | Authentication failure |
| `WS_EINTERNAL` | -9 | Internal error |

## Known limitations

These are known gaps as of version 0.1.0. They are good first contributions.

1. **Native TLS does not carry data.** `server_init()` creates a TLS 1.2+
   context and loads the certificate and key, and `server_accept_client()`
   runs `SSL_accept()`. After that, however, all reads and writes still use
   the raw socket (`socket_recv`/`socket_send`) instead of
   `SSL_read`/`SSL_write`, so `wss://` does not work. The TLS handshake also
   runs in the parent process, where a slow client blocks `accept()`.
2. **permessage-deflate is always advertised.** `websocket_accept()` detects
   whether the client offered the extension, but always includes it in the
   `101` response. Clients that did not offer it must fail the connection
   (RFC 6455 section 9.1), so tools such as websocat, or Python `websockets`
   with compression disabled, cannot connect. Browsers are not affected.
3. **Token routing is not connected.** `token_file` is parsed and
   `token_auth.c` implements lookup, but `handle_client()` always uses the
   single configured target.
4. **Metrics are not exposed.** `metrics.c` can format Prometheus and JSON
   output, but nothing collects metrics or serves an endpoint. With the
   process-per-connection model, counters would also need shared memory.
5. **The connection pool is inert.** The pool is created, but connections are
   never returned to it (`conn_pool_put()` is not called), so every session
   opens a new target connection. That is the correct behavior for stateful
   protocols like VNC, so the pool should probably stay disabled for them.
6. **Configuration precedence.** Config-file values override command-line
   options (see above).
7. **`max_connections` is not enforced** on the process count.
8. **HTTP is minimal.** There is no keep-alive, no `HEAD`/`Range` support and
   no directory listing. Errors are plain status lines.
9. **No automated tests.** There are no unit, integration or fuzz tests yet.
10. **Deprecated OpenSSL API.** `SHA1_Init`, `SHA1_Update` and `SHA1_Final`
    trigger deprecation warnings on OpenSSL 3.x. The `EVP_Digest*` API
    replaces them.

## Build system

CMake 3.10 or newer, C11:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build                 # binary, plus docs if Doxygen is found
cmake --build build --target docs   # API reference only
sudo cmake --install build          # bin/ws2socket, share/man/man1/ws2socket.1
```

The project is compiled with `-Wall -Wextra -Wpedantic -Wstrict-prototypes` and
links against `OpenSSL::SSL`, `OpenSSL::Crypto`, `ZLIB::ZLIB` and `pthread`.
When Doxygen is available, the `docs` target is part of `ALL`.

## Yocto integration

ws2socket builds with the standard `cmake` class. A minimal recipe
(`recipes-connectivity/ws2socket/ws2socket_0.1.0.bb`):

```bitbake
SUMMARY = "WebSocket to TCP socket proxy"
DESCRIPTION = "Lightweight C replacement for websockify, suitable for noVNC"
LICENSE = "LGPL-3.0-or-later"
LIC_FILES_CHKSUM = "file://COPYING;md5=<run md5sum COPYING>"

SRC_URI = "git://<repository-url>;protocol=https;branch=main"
SRCREV = "<commit>"
S = "${WORKDIR}/git"

DEPENDS = "openssl zlib"

inherit cmake

# Skip the Doxygen target, even if doxygen-native happens to be available
EXTRA_OECMAKE = "-DCMAKE_DISABLE_FIND_PACKAGE_Doxygen=ON"

FILES:${PN} += "${mandir}/man1/ws2socket.1"
```

`cmake_do_install` already installs the binary and the man page, so no custom
`do_install` is needed. Fill in the checksum with `md5sum COPYING`. The recipe
has been written for Scarthgap (5.0) syntax.

## Coding conventions

- C11, 4-space indentation, K&R braces for control flow, and the function's
  opening brace on its own line.
- Every file starts with a Doxygen header (`@file`, `@brief`, `@author`,
  `@copyright`) and an `SPDX-License-Identifier` line.
- Every public function has a Doxygen block with `@brief`, `@param` and
  `@return`.
- Use `strlcpy()` from `utils.c` for bounded copies, and never `strcpy`.
- Return `WS_*` codes, and log at the point where the error is detected.

## Roadmap

In rough priority order:

1. Make native TLS work (`SSL_read`/`SSL_write` throughout, handshake in the
   child).
2. Advertise permessage-deflate only when the client offers it.
3. Make command-line options override the config file.
4. Wire in token-based target selection (websockify-compatible
   `?token=` / `path` syntax).
5. Add a test suite (frame codec, handshake, config parser) and fuzzing of the
   HTTP and frame parsers.
6. Move SHA-1 to the EVP API.
7. Add an optional `/metrics` endpoint.
8. Consider an `epoll` event loop for very high connection counts.
