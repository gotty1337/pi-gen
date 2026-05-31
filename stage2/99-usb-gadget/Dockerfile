FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    # Native build tools
    gcc \
    make \
    build-essential \
    # Cross-compiler: arm64 (Pi Zero 2W default)
    gcc-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    # Cross-compiler: armhf (32-bit Raspberry Pi OS)
    gcc-arm-linux-gnueabihf \
    binutils-arm-linux-gnueabihf \
    # Linux kernel headers (USB FunctionFS, ch9 — arch-independent)
    linux-libc-dev \
    # Kernel headers in the cross-compiler sysroots (needed by aarch64/armhf gcc)
    linux-libc-dev-arm64-cross \
    linux-libc-dev-armhf-cross \
    # Handy dev tools
    gdb-multiarch \
    file \
    ca-certificates \
    curl \
    git \
    && rm -rf /var/lib/apt/lists/*
