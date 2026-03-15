FROM debian:bookworm-slim

RUN dpkg --add-architecture arm64 && \
    apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    make \
    libpipewire-0.3-dev:arm64 \
    libspa-0.2-dev:arm64 \
    libjack-jackd2-dev:arm64 \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

ENV CROSS_PREFIX=aarch64-linux-gnu-
ENV PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig
ENV PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig
ENV PKG_CONFIG_SYSROOT_DIR=/
WORKDIR /build
