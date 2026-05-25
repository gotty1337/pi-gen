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
    # Vim + plugins bootstrap
    vim \
    curl \
    git \
    && rm -rf /var/lib/apt/lists/*

# Install vim-plug
RUN curl -fLo /root/.vim/autoload/plug.vim --create-dirs \
    https://raw.githubusercontent.com/junegunn/vim-plug/master/plug.vim

# Write vimrc
COPY vimrc /root/.vimrc

# Copy pi-gen folder
COPY --chmod=755 files /usb-gadget

# Install plugins non-interactively
RUN vim -E -s -u /root/.vimrc +PlugInstall +qall || true

WORKDIR /usb-gadget/

