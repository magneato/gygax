#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"

pick_compiler() {
    if [ -n "${CXX:-}" ]; then echo "$CXX"; return; fi
    for c in clang++-21 clang++-20 clang++-19 clang++ clang++-18; do
        if command -v "$c" >/dev/null 2>&1; then echo "$c"; return; fi
    done
    echo "no clang++ found; install clang-18 or newer" >&2
    exit 1
}

configure() {
    local cxx
    cxx="$(pick_compiler)"
    cmake -G Ninja -B "$BUILD_DIR" -S . -DCMAKE_CXX_COMPILER="$cxx" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "$@"
}

case "${1:-build}" in
    clean)
        rm -rf "$BUILD_DIR"
        echo "removed $BUILD_DIR"
        ;;
    configure)
        shift
        configure "$@"
        ;;
    build)
        shift || true
        configure "$@"
        cmake --build "$BUILD_DIR"
        echo
        echo "built:"
        echo "  $BUILD_DIR/gygax          service, CLI and self-test  (try: $BUILD_DIR/gygax doctor)"
        echo "  $BUILD_DIR/libgygax_c.so  C ABI for Python and other languages"
        echo "  $BUILD_DIR/arrival        multi-agent and sim-to-real example"
        ;;
    test)
        configure
        cmake --build "$BUILD_DIR"
        ctest --test-dir "$BUILD_DIR" --output-on-failure
        ;;
    install)
        configure
        cmake --build "$BUILD_DIR"
        cmake --install "$BUILD_DIR" --prefix "${PREFIX:-/usr/local}"
        ;;
    *)
        echo "usage: $0 [build|test|install|configure|clean] [extra cmake -D options]" >&2
        exit 2
        ;;
esac
