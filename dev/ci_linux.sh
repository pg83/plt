#!/usr/bin/env sh
# Run inside the stock distribution container selected by CI.
set -eu
mode=${1:-test}
toolchain=${2:-clang}
if [ -f /etc/alpine-release ]; then
    apk add --no-cache bash binutils clang21 g++ linux-headers lld python3 pkgconf \
        wayland-dev wayland-protocols libxkbcommon-dev
    export CC=clang-21 CXX=clang++-21
    export LDFLAGS="${LDFLAGS:-} -fuse-ld=lld"
else
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install --yes --no-install-recommends python3 pkg-config \
        libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev
    if [ "$toolchain" = gcc ]; then
        export CC=gcc CXX=g++
    else
        apt-get install --yes --no-install-recommends clang libclang-rt-dev lld llvm
        export CC=clang CXX=clang++
        export LDFLAGS="${LDFLAGS:-} -fuse-ld=lld"
    fi
fi
exec bash dev/ci.sh "$mode"
