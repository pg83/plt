#!/usr/bin/env bash
# Build libstd and plt with the same toolchain and instrumentation.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD
mode=${1:-test}
std_source=${LIBSTD_SOURCE:-$root/.deps/libstd}
std_source=$(cd "$std_source" && pwd)
build_dir="$root/.build/ci-$mode"
jobs=${CI_JOBS:-$(getconf _NPROCESSORS_ONLN)}

case "$mode" in
    build|test) ;;
    asan|ubsan)
        "$CXX" --version | grep -qi clang
        sanitizer=address
        if [[ "$mode" == ubsan ]]; then sanitizer=undefined; fi
        export CXXFLAGS="${CXXFLAGS:-} -g -fsanitize=$sanitizer -fno-sanitize-recover=all -fno-omit-frame-pointer"
        export LDFLAGS="${LDFLAGS:-} -fsanitize=$sanitizer"
        export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1
        export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
        ;;
    coverage)
        "$CXX" --version | grep -qi clang
        export CXXFLAGS="${CXXFLAGS:-} -g -fprofile-instr-generate -fcoverage-mapping"
        export CPPFLAGS="${CPPFLAGS:-} -D__LLVM_INSTR_PROFILE_GENERATE=1"
        export LDFLAGS="${LDFLAGS:-} -fprofile-instr-generate"
        mkdir -p "$build_dir/profiles"
        export LLVM_PROFILE_FILE="$build_dir/profiles/%m-%p.profraw"
        ;;
    *) echo "usage: $0 build|test|asan|ubsan|coverage" >&2; exit 2 ;;
esac

"$CXX" --version
(
    cd "$std_source"
    python3 ./build -B "$build_dir/std" -j "$jobs" libstd
)
export CPPFLAGS="${CPPFLAGS:-} -I$std_source"
export LDFLAGS="${LDFLAGS:-} -L$build_dir/std"
if [[ "$mode" == build ]]; then
    python3 ./build -B "$build_dir/plt" -j "$jobs" plt plt_unit_tests plt_wayland_integration_tests
else
    python3 ./build -B "$build_dir/plt" -j "$jobs" test
fi
if [[ "$mode" == coverage ]]; then
    python3 dev/ci_coverage.py "$build_dir/plt" "$build_dir/profiles" "$root/.build/coverage"
fi
