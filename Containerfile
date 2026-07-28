FROM docker.io/alpine:3.20

RUN apk add --no-cache \
    gcc g++ make binutils \
    musl-dev linux-headers \
    nasm \
    bison flex \
    ncurses-dev \
    cpio \
    e2fsprogs dosfstools mtools \
    xorriso \
    qemu-system-x86_64 \
    pkgconfig \
    gawk file \
    curl wget tar \
    bash coreutils findutils \
    git

RUN printf '#!/bin/sh\nexec gcc "$@"\n' > /usr/local/bin/musl-gcc \
    && chmod +x /usr/local/bin/musl-gcc

WORKDIR /src
