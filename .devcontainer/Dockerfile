FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    # Native build tools
    gcc \
    make \
    # Cross-compiler: arm64 (Pi Zero 2W default)
    gcc-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    # Cross-compiler: armhf (32-bit Raspberry Pi OS)
    gcc-arm-linux-gnueabihf \
    binutils-arm-linux-gnueabihf \
    # Linux kernel headers (USB FunctionFS, ch9 — arch-independent)
    linux-libc-dev \
    # Handy dev tools
    gdb \
    file \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
