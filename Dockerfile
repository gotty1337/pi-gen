ARG BASE_IMAGE=debian:bookworm
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        git \
        vim \
        parted \
        quilt \
        coreutils \
        qemu-user-binfmt \
        debootstrap \
        zerofree \
        zip \
        dosfstools \
        e2fsprogs \
        libarchive-tools \
        libcap2-bin \
        rsync \
        grep \
        udev \
        xz-utils \
        curl \
        xxd \
        file \
        kmod \
        bc \
        binfmt-support \
        ca-certificates \
        fdisk \
        gpg \
        debian-archive-keyring \
        pigz \
        arch-test \
    && rm -rf /var/lib/apt/lists/*

RUN git config --global --add safe.directory /pi-gen

COPY . /pi-gen/

RUN find /pi-gen -name "*.sh" -exec chmod +x {} +

WORKDIR /pi-gen

VOLUME ["/pi-gen/work", "/pi-gen/deploy"]

CMD ["/bin/bash"]
