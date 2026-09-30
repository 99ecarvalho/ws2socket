# Copyright (c) 2026 Eduardo Correia <ecorreia@apliant.com.br>
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Multi-stage build for ws2socket. Build from the repository root:
#
#   docker build -f docker/Dockerfile -t ws2socket .

ARG DEBIAN_RELEASE=bookworm

# ---------------------------------------------------------------------------
# Build stage: compile ws2socket
# ---------------------------------------------------------------------------
FROM debian:${DEBIAN_RELEASE}-slim AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        libssl-dev \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B /build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_DISABLE_FIND_PACKAGE_Doxygen=ON \
    && cmake --build /build --parallel \
    && DESTDIR=/install cmake --install /build \
    && strip /install/usr/bin/ws2socket

# ---------------------------------------------------------------------------
# Runtime stage: binary plus its shared libraries only
# ---------------------------------------------------------------------------
FROM debian:${DEBIAN_RELEASE}-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libssl3 \
        zlib1g \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --no-create-home --shell /usr/sbin/nologin ws2socket

COPY --from=build /install/usr/bin/ws2socket /usr/bin/ws2socket
COPY --from=build /install/usr/share/man/man1/ws2socket.1 /usr/share/man/man1/ws2socket.1
COPY ws2socket.conf.example /etc/ws2socket/ws2socket.conf.example
COPY COPYING COPYING.LESSER /usr/share/doc/ws2socket/

LABEL org.opencontainers.image.title="ws2socket" \
      org.opencontainers.image.description="WebSocket to TCP socket proxy" \
      org.opencontainers.image.source="https://github.com/99ecarvalho/ws2socket" \
      org.opencontainers.image.licenses="LGPL-3.0-or-later"

USER ws2socket
EXPOSE 6080

# Arguments passed to "docker run" are handed to ws2socket, for example:
#   docker run --rm -p 6080:6080 ws2socket --target vnc-host:5900
ENTRYPOINT ["/usr/bin/ws2socket"]
CMD ["--help"]
