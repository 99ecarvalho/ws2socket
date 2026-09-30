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

- The parent only accepts connections and forks. It counts live children,
  which a `SIGCHLD` handler decrements as it reaps them. When
  `[server] max_connections` children are alive, new clients get
  `503 Service Unavailable` without a fork.
- Each child sets the `[server] socket_timeout` receive and send timeouts on
  its socket, performs the TLS handshake if TLS is configured, and then
  handles exactly one HTTP request or one WebSocket session before exiting. A
  slow or silent client therefore only ever blocks its own process.
- Inside a child, `proxy_forward()` runs a `select()` loop over two
  descriptors: the client socket and the target socket. Bytes that OpenSSL
  has already decrypted are invisible to `select()`, so the loop checks
  `SSL_pending()` first. An idle `select()` timeout does not close the
  session.

Consequences:

- Sessions are fully isolated, and there is no shared mutable state between
  them.
- Per-session memory is one process. That is cheap with copy-on-write, but a
  very high connection count costs more than it would with an event loop.
- Anything that is meant to span sessions (metrics, connection pooling) would
  need shared memory or IPC, because each child has its own copy. See
  [Known limitations](#known-limitations).
- Read-only state loaded at startup (configuration, the token table, the TLS
  context) is simply inherited by each child.

## Request flow

`handle_client()` in `src/ws2socket.c` runs in the child. It loops over the
HTTP requests of a keep-alive connection until the client closes it, the
5-second idle timeout expires, 100 requests have been served, or a WebSocket
upgrade takes the connection over:

1. `server_recv_request()` reads and parses one request (request line and up
   to 32 headers). Bytes that arrive after the headers are kept for the next
   request, so pipelined requests work. Malformed or oversized requests get
   `400`, `414` or `431`, followed by a lingering close: the server stops
   writing and drains the client's input briefly, so the error response is not
   lost to a TCP reset.
2. With `--auth-file`, `check_auth()` verifies the `Authorization` header
   (`auth_check()` in `auth.c`). Missing or wrong credentials get
   `401 Unauthorized` with a `WWW-Authenticate` challenge, and the loop goes on
   with the next request if the connection is kept alive.
3. A WebSocket upgrade (`http_is_websocket_upgrade()`) is passed to
   `handle_websocket()`, which runs steps 4 to 8.
   Otherwise, `/metrics` (with `--metrics`) is answered from the shared
   counters, a web root is served by `http_serve_file()`, and without a web
   root the reply is `426 Upgrade Required`.
4. The target is chosen. With a token file, the token is taken from the
   request path (`token_auth_extract_from_path()`) and looked up; a missing
   or unknown token gets `403 Forbidden`. Otherwise the configured target is
   used.
5. `proxy_connect_target()` opens the TCP connection to the target, bounded by
   `[proxy] socket_timeout`. If this fails, the client gets
   `502 Bad Gateway`. Connecting *before* the handshake means a client never
   sees a WebSocket that opens and immediately closes.
6. `websocket_accept()` computes
   `Sec-WebSocket-Accept = base64(SHA1(key + GUID))` and sends
   `101 Switching Protocols`. It accepts permessage-deflate only if the client
   offered it.
7. `proxy_forward()` copies data in both directions until either side closes:
   - WebSocket to TCP: `websocket_recv_frame()` reads one frame. It returns a
     complete message (reassembled and decompressed if needed), or 0 after
     consuming a control frame or a non-final fragment. The session ends when
     the WebSocket state leaves `WS_STATE_OPEN`.
   - TCP to WebSocket: whatever is read from the target (up to 64 KiB) is sent
     as one binary frame. When the target closes, a close frame (1000) is sent
     to the client.
8. On exit, the session duration is logged. With `--verbose`, byte counts and
   the compression ratio are logged too.

All client I/O goes through `io_send_all()`, `io_recv()` and
`io_recv_exact()` in `utils.c`. They use `SSL_write()`/`SSL_read()` when the
connection has a TLS session and `send()`/`recv()` otherwise, so the HTTP,
handshake and framing code is the same for `ws://` and `wss://`.

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
| Server | `include/server.h`, `src/server.c` | Listening socket, TLS context, `accept()` and fork loop, connection limit, per-child TLS handshake, HTTP request reading |
| HTTP | `src/http_server.c` | Upgrade detection, header lookup, static files, MIME types |
| WebSocket | `include/websocket.h`, `src/websocket.c`, `src/websocket_impl.c` | RFC 6455 handshake, frame codec, masking, fragmentation, control frames, permessage-deflate |
| Proxy | `include/proxy.h`, `src/proxy.c` | Target connection, bidirectional `select()` forwarding, statistics |
| Utilities | `include/utils.h`, `src/utils.c` | Safe strings, `host:port` parsing, Base64, SHA-1, random bytes, socket helpers, TLS-aware `io_*` helpers, circular buffer |
| Logging | `include/logging.h`, `src/logging.c` | Levels, and console, file and syslog targets (mutex-protected) |
| Token auth | `include/token_auth.h`, `src/token_auth.c` | Token-file parser, hash-table lookup, token extraction from the request path, periodic reload |
| Authentication | `include/auth.h`, `src/auth.c` | htpasswd-style password file, HTTP Basic parsing, `crypt_r()` verification, reload on change |
| Connection pool | `include/conn_pool.h`, `src/conn_pool.c` | LRU pool of target connections (inactive, see limitations) |
| Metrics | `include/metrics.h`, `src/metrics.c` | Counters in shared memory, updated atomically from every process; Prometheus text export |
| Common | `include/common.h` | Error codes, limits, WebSocket opcodes and states |

### WebSocket details

- Supports all three payload-length encodings (7-bit, 16-bit and 64-bit).
  Headers, lengths, masks and payloads are read with `io_recv_exact()`, so
  frames split across TCP segments or TLS records are handled.
- Unmasks client frames. Server frames are sent unmasked, as RFC 6455
  requires.
- Fails the connection with close code 1002 for unmasked client frames, set
  RSV2/RSV3 bits, RSV1 without negotiated compression, fragmented or
  oversized control frames, unknown opcodes and out-of-order continuation
  frames.
- Reassembles continuation frames into one message before delivery. A
  message larger than `[proxy] buffer_size` (default 1 MiB) is refused with
  close code 1009.
- Answers ping with pong, and echoes the peer's close code.
- permessage-deflate is negotiated only when the client offers it. It uses raw
  DEFLATE (`windowBits = -15`) with `server_no_context_takeover` and
  `client_no_context_takeover`. A compressed message is inflated after
  reassembly, and output that would exceed `buffer_size` is refused (1009)
  rather than truncated. Outgoing frames are not compressed.
- Data from the target is always sent as **binary** frames, which is what
  noVNC and websockify clients expect.

### HTTP details

- HTTP/1.1 with keep-alive (the default for 1.1, opt-in for 1.0). A request
  that announces a body (`Content-Length` or `Transfer-Encoding`) is answered
  and the connection is then closed, because bodies are never read.
- `GET` and `HEAD` are served; other methods get `405` with `Allow`.
- The path is percent-decoded (`http_decode_path()`), and the query string and
  fragment are dropped. Malformed escapes and `%00` get `400`, and `.` or `..`
  segments get `403`. The path is then joined to the web root.
- A directory without a trailing `/` is redirected (`301`); with one,
  `index.html` is served. Directories are never listed.
- Responses carry `Date`, `Server`, `Last-Modified`, `Cache-Control: no-cache`
  (browsers revalidate, so noVNC upgrades show up at once), `Accept-Ranges`
  and `X-Content-Type-Options: nosniff`.
- `If-Modified-Since` gets `304`. A single `bytes=` range gets `206`, or
  `416` when unsatisfiable. Multiple ranges, other units and requests with
  `If-Range` get the whole file.
- MIME types cover HTML, CSS, JavaScript (`js`, `mjs`), JSON, source maps,
  web manifests, XML, text, PNG, JPEG, GIF, SVG, ICO, WebP, WASM, MP3, Ogg,
  WAV and web fonts, with `charset=utf-8` for text types. Everything else is
  `application/octet-stream`.
- Status codes: `101`, `200`, `206`, `301`, `304`, `400`, `401`, `403`, `404`,
  `405`, `414`, `416`, `426`, `431`, `502`, `503`.

### Authentication details

- The password file has `user:hash` lines. Only hashes starting with `$`
  (modern `crypt(3)` schemes) are accepted; plaintext and DES entries are
  skipped with a warning.
- `auth_check()` decodes the Basic credentials with a strict Base64 decoder,
  and verifies the password with `crypt_r()` and a constant-time comparison.
  Unknown users are hashed against a real entry too, so timing does not reveal
  which names exist. A failed attempt sleeps for one second in its own
  process.
- The last accepted `Authorization` value is cached per connection, so a
  keep-alive connection pays for the (deliberately slow) bcrypt hash once.
- The file is re-read when its modification time changes.
- libcrypt is optional at build time (`WS2SOCKET_HAVE_CRYPT`).

### Metrics details

- `metrics_init()` maps an anonymous `MAP_SHARED` region before the first
  `fork()`, so every child process updates the same counters.
- Updates are relaxed `__atomic` operations, and are async-signal-safe: the
  `SIGCHLD` handler decrements `connections_active`. Counters are 64-bit where
  the CPU has lock-free 64-bit atomics, and word-sized otherwise, because a
  lock-based fallback would be neither signal-safe nor shared between
  processes.
- The session gauge is decremented by the child when its session ends. A child
  killed mid-session therefore leaves the gauge one too high until restart.

### Logging

| Level | Config value | Use |
|---|---|---|
| `WS_LOG_DEBUG` | `debug` | Frames, headers, per-read detail (`--verbose`) |
| `WS_LOG_INFO` | `info` | Connections, configuration summary (the default) |
| `WS_LOG_WARN` | `warning` | Recoverable oddities, rejected clients |
| `WS_LOG_ERROR` | `error` | Failed operations |
| `WS_LOG_CRITICAL` | `critical` | Startup failures |

The levels are prefixed with `WS_` because `<syslog.h>` defines its own
`LOG_DEBUG`, `LOG_INFO` and so on, with different values.

The targets `LOG_TARGET_CONSOLE` (stderr), `LOG_TARGET_FILE` and
`LOG_TARGET_SYSLOG` can be combined. The syslog facility is `LOG_LOCAL0`.
Warnings and errors raised before `log_init()`, such as while parsing
arguments, are written to stderr.

## Configuration handling

`main()` builds the configuration in this order:

1. `config_init_defaults()`: listen `0.0.0.0:6080`, 1 MiB maximum message
   size, up to 1024 connections, 60 s timeouts, log level `info` to the
   console.
2. `config_parse_args()`: a first pass over the command line. It rejects
   invalid options and values, and finds `--config`.
3. If `--config` was given, the defaults are restored, `config_load_file()`
   reads the INI file, and `config_parse_args()` runs again. As a result,
   **command-line options override file values**.
4. Relative paths (certificate, key, web root, token file, PID file and log
   file) are made absolute, because `--daemon` changes directory to `/`.
5. `config_validate()`: requires a target or a token file, and both a
   certificate and a key if either is set.

Invalid values in the file (a bad port or address, a non-boolean for a
boolean key, an unknown log level, a negative number) stop startup with a
`file:line` message.

The recognized sections and keys are:

| Section | Keys |
|---|---|
| `[general]` | `daemon`, `pid_file`, `token_file` |
| `[server]` | `listen`, `port`, `cert_file`, `key_file`, `max_connections`, `socket_timeout`, `web_root`, `auth_file`, `auth_realm`, `metrics` |
| `[proxy]` | `target`, `buffer_size`, `socket_timeout` (`max_connections` is accepted but has no effect) |
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

1. **Static files only.** The HTTP server has no directory listings, response
   compression, multi-range responses or request bodies.
2. **Basic authentication only.** There are no sessions, logout, per-user
   targets or client certificates. Any authenticated user may use any token.
3. **The connection pool is inactive.** The pool is created, but connections
   are never returned to it (`conn_pool_put()` is not called), so every
   session opens a new target connection. That is the correct behavior for
   stateful protocols like VNC, so the pool should probably stay unused for
   them.
4. **Outgoing frames are not compressed.** VNC data compresses poorly, so
   this is deliberate. The unfinished `websocket_send_frame_FIXME_compressed()`
   is where it would go.
5. **No unit tests or fuzzing harness in the tree.** The integration tests
   cover behavior end to end, but the parsers deserve dedicated fuzz targets.

## Build system

CMake 3.10 or newer, C11:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build                 # binary, plus docs if Doxygen is found
cmake --build build --target docs   # API reference only
sudo cmake --install build          # bin/ws2socket, share/man/man1/ws2socket.1
ctest --test-dir build              # integration tests (needs python3-websockets)
```

The project is compiled with `-Wall -Wextra -Wpedantic -Wstrict-prototypes` and
links against `OpenSSL::SSL`, `OpenSSL::Crypto`, `ZLIB::ZLIB` and `pthread`.
When Doxygen is available, the `docs` target is part of `ALL`.

### Tests

[`tests/test_integration.py`](../tests/test_integration.py) runs the built
binary against local TCP services and talks to it with the Python
`websockets` client, over plain TCP and TLS. It is registered with CTest when
`python3` is found. It can also run on its own, optionally filtered by name:

```bash
python3 tests/test_integration.py build/ws2socket -k tls
```

The suite runs cleanly under AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan
# Leak checking is off: children exit without freeing process-lifetime state
ASAN_OPTIONS=detect_leaks=0 python3 tests/test_integration.py build-asan/ws2socket
```

## Yocto integration

ws2socket builds with the standard `cmake` class. A minimal recipe
(`recipes-connectivity/ws2socket/ws2socket_0.1.0.bb`):

```bitbake
SUMMARY = "WebSocket to TCP socket proxy"
DESCRIPTION = "Lightweight C replacement for websockify, suitable for noVNC"
LICENSE = "LGPL-3.0-or-later"
LIC_FILES_CHKSUM = "file://COPYING.LESSER;md5=3000208d539ec061b899bce1d9ce9404 \
                    file://COPYING;md5=1ebbd3e34237af26da5dc08a4e440464"

SRC_URI = "git://github.com/99ecarvalho/ws2socket.git;protocol=https;branch=main"
SRCREV = "<commit or tag to build>"
S = "${WORKDIR}/git"

DEPENDS = "openssl zlib virtual/crypt"

inherit cmake

# Skip the Doxygen target, even if doxygen-native happens to be available
EXTRA_OECMAKE = "-DCMAKE_DISABLE_FIND_PACKAGE_Doxygen=ON"

FILES:${PN} += "${mandir}/man1/ws2socket.1"
```

`cmake_do_install` already installs the binary and the man page, so no custom
`do_install` is needed. If the license files change, update the checksums
with `md5sum COPYING COPYING.LESSER`. The recipe uses Scarthgap (5.0)
syntax.

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

1. Add libFuzzer targets for the HTTP request parser, the frame decoder, the
   Base64 decoder and the configuration parser, plus unit tests for them.
2. Client-certificate (mutual TLS) authentication, and per-user token
   restrictions.
3. Compress static files on the fly or serve pre-compressed `.gz` files.
4. Consider an `epoll` event loop for very high connection counts.
