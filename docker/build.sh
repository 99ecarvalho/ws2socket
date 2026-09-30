#!/bin/sh

# Copyright (c) 2026 Eduardo Correia <ecorreia@apliant.com.br>
# SPDX-License-Identifier: LGPL-3.0-or-later

# Build the ws2socket Docker image and check that the binary runs.
#
# Usage: docker/build.sh [IMAGE_TAG]    (default tag: ws2socket:latest)

set -eu

IMAGE="${1:-ws2socket:latest}"

# The build context is the repository root, one level above this script
cd "$(dirname "$0")/.."

docker build -f docker/Dockerfile -t "$IMAGE" .

echo
echo "Smoke test:"
docker run --rm "$IMAGE" --version

echo
echo "Built $IMAGE. Run it with, for example:"
echo "  docker run --rm -p 6080:6080 $IMAGE --target <vnc-host>:5900"
