# Docker image for ws2socket

This directory builds a small container image for ws2socket. It uses two
stages:

1. A **build stage** based on `debian:bookworm-slim` compiles ws2socket with
   CMake.
2. A **runtime stage** contains only the stripped binary, the OpenSSL and zlib
   runtime libraries, the man page and the license files. The image is about
   80 MB.

The container runs as the unprivileged `ws2socket` user and exposes port
`6080`.

## Building

The build context must be the repository root. Either use the helper script:

```bash
docker/build.sh                # tags ws2socket:latest
docker/build.sh ws2socket:dev  # custom tag
```

or run `docker build` yourself from the repository root:

```bash
docker build -f docker/Dockerfile -t ws2socket .
```

The script builds the image and then runs `ws2socket --version` inside it as a
smoke test. To build against another Debian release, pass
`--build-arg DEBIAN_RELEASE=trixie`.

## Running

Arguments after the image name are passed straight to `ws2socket`. With no
arguments, the image prints `--help`.

```bash
docker run --rm -p 6080:6080 ws2socket --target 192.168.1.50:5900
```

Keep the default listen address (`0.0.0.0:6080`) inside the container, and
choose the public port with `-p`.

### Reaching a service on the Docker host

Inside the container, `127.0.0.1` is the container itself, not the host. To
reach a VNC server running on the host, use one of these:

```bash
# Option 1: map host.docker.internal to the host gateway
docker run --rm -p 6080:6080 --add-host=host.docker.internal:host-gateway \
    ws2socket --target host.docker.internal:5900

# Option 2 (Linux only): share the host network namespace
docker run --rm --network host \
    ws2socket --listen 127.0.0.1:6080 --target 127.0.0.1:5900
```

With option 1, the VNC server must listen on an address the container can
reach, not only on `127.0.0.1`.

### Serving noVNC

Mount a noVNC checkout read-only and point `--web-root` at it:

```bash
git clone --depth 1 https://github.com/novnc/noVNC.git
docker run --rm -p 6080:6080 \
    -v "$PWD/noVNC:/usr/share/novnc:ro" \
    ws2socket --target vnc-host:5900 --web-root /usr/share/novnc
```

Then open `http://localhost:6080/vnc.html`.

### Serving over TLS

Mount the certificate and key read-only. The container runs as the
unprivileged `ws2socket` user (UID 999), so both files must be readable by
that UID, for example with `chown 999 privkey.pem && chmod 0400 privkey.pem`:

```bash
docker run --rm -p 6080:6080 \
    -v /etc/ws2socket/tls:/tls:ro \
    ws2socket --target vnc-host:5900 \
              --cert /tls/fullchain.pem --key /tls/privkey.pem
```

Clients then connect with `https://` and `wss://`.

### Requiring a password and exposing metrics

Mount the password file (readable by UID 999) and enable the options you
need:

```bash
docker run --rm -p 6080:6080 \
    -v /etc/ws2socket/tls:/tls:ro \
    -v /etc/ws2socket/htpasswd:/etc/ws2socket/htpasswd:ro \
    ws2socket --target vnc-host:5900 \
              --cert /tls/fullchain.pem --key /tls/privkey.pem \
              --auth-file /etc/ws2socket/htpasswd --metrics
```

The image is built with libcrypt, so bcrypt, SHA-crypt and yescrypt hashes
all work.

### Using a configuration file

An annotated example is included at `/etc/ws2socket/ws2socket.conf.example`.
Mount your own file and pass it with `--config`:

```bash
docker run --rm -p 6080:6080 \
    -v "$PWD/ws2socket.conf:/etc/ws2socket/ws2socket.conf:ro" \
    ws2socket --config /etc/ws2socket/ws2socket.conf
```

Inside a container, log to the console (`console = true`) and let Docker
collect the output with `docker logs`. Do not set `daemon = true`, because the
container stops as soon as its main process exits.

## Docker Compose example

```yaml
services:
  ws2socket:
    build:
      context: .
      dockerfile: docker/Dockerfile
    command: ["--target", "vnc:5900", "--web-root", "/usr/share/novnc"]
    ports:
      - "6080:6080"
    volumes:
      - ./noVNC:/usr/share/novnc:ro
    restart: unless-stopped
```
