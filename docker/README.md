# Docker Setup for ws2socket

This directory contains the necessary files to build and run the `ws2socket` project inside a Docker container.

## Files

- `Dockerfile`: Defines the Docker image and build steps.
- `build.sh`: A script to build the Docker image and run the container.

## Usage

1. Build the Docker image:
   ```bash
   ./build.sh
   ```

2. Run the container:
   ```bash
   docker run --rm ws2socket
   ```