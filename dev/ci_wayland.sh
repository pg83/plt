#!/usr/bin/env bash
# Keep queued object arguments balanced when an application retires a surface
# before dispatch. Upstream validate_closure_objects discards the pointer but
# leaves the reference acquired by increase_closure_args_refcount behind.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
work="$root/.build/wayland-dependency"
mkdir -p "$work"
archive="$work/wayland-1.26.0.tar.bz2"
curl --fail --location --retry 3 \
    https://gitlab.freedesktop.org/wayland/wayland/-/archive/1.26.0/wayland-1.26.0.tar.bz2 \
    --output "$archive"
echo "ebf5fff1c8b11c24ceec74ff3047aefdb07efee8ce09bf3b856975aba3540d15  $archive" | sha256sum -c
tar -xjf "$archive" -C "$work"
patch -d "$work/wayland-1.26.0" -p1 < "$root/dev/wayland-queued-proxy.patch"
meson setup "$work/build" "$work/wayland-1.26.0" \
    --prefix "$work/install" --libdir lib --buildtype release \
    -Dtests=false -Ddocumentation=false -Ddtd_validation=false
meson compile -C "$work/build" -j "${CI_JOBS:-4}"
meson install -C "$work/build"
