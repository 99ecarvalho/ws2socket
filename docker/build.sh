#!/bin/bash

# Ensure the script is run from the top-level directory
cd "$(dirname "$0")/.."

# Build the Docker image
docker build -t ws2socket -f docker/Dockerfile .

# Run the Docker container with read-only source code and writable build directory
docker run --rm \
  -v "$(pwd):/app:ro" \
  -v "$(pwd)/build:/app/build" \
  ws2socket