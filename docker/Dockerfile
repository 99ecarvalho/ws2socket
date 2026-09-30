# Copyright (c) 2026 Eduardo Correia <ecorreia@apliant.com.br>
# SPDX-License-Identifier: LGPL-3.0-or-later

# Dockerfile for building the ws2socket project

# Use an official lightweight Python image as a base
FROM python:3.9-slim

# Set the working directory in the container
WORKDIR /app

# Copy the project files into the container
COPY ../ /app

# Ensure the build directory is writable
RUN mkdir -p /app/build && chmod -R 777 /app/build

# Install necessary build tools and libraries
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libssl-dev \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Build the project
RUN rm -rf /app/build/* && cd /app/build && cmake .. && make

# Copy the example configuration file to the expected location
RUN mkdir -p /etc/ws2socket && cp /app/ws2socket.conf.example /etc/ws2socket/ws2socket.conf

# Expose the port used by the server
EXPOSE 6080

# Set the default command to execute the built binary with the configuration file
CMD ["/app/build/ws2socket", "--config", "/etc/ws2socket/ws2socket.conf"]