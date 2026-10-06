#!/usr/bin/env bash
set -euo pipefail

need_ok=1

version_ge() {
    [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" = "$2" ]
}

check() {
    local name="$1" cmd="$2" min="$3"
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "missing   $name ($cmd)"
        need_ok=0
        return
    fi
    local ver
    ver="$("$cmd" --version 2>/dev/null | head -n1 | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -n1)"
    if [ -n "$min" ] && ! version_ge "$ver" "$min"; then
        echo "too old   $name $ver (need >= $min)"
        need_ok=0
    else
        echo "ok        $name $ver"
    fi
}

cxx=""
for c in clang++-19 clang++-18 clang++; do
    if command -v "$c" >/dev/null 2>&1; then cxx="$c"; break; fi
done

if [ -n "$cxx" ]; then check "clang++" "$cxx" 18.0; else echo "missing   clang++ (need >= 18)"; need_ok=0; fi
scandeps=""
for c in clang-scan-deps-19 clang-scan-deps-18 clang-scan-deps; do
    if command -v "$c" >/dev/null 2>&1; then scandeps="$c"; break; fi
done
if [ -n "$scandeps" ]; then
    echo "ok        clang-scan-deps ($scandeps)"
else
    echo "missing   clang-scan-deps (package clang-tools-18; Ninja needs it to scan C++26 module dependencies)"
    need_ok=0
fi
check "cmake" cmake 3.28
check "ninja" ninja ""
check "python3 (optional, bindings)" python3 3.9
check "curl (optional, smoke tests)" curl ""

if [ "$need_ok" -eq 0 ]; then
    echo
    echo "Install the missing tools, for example on Ubuntu 24.04:"
    echo "  sudo apt-get install clang-18 clang-tools-18 cmake ninja-build python3 curl libeigen3-dev libgmp-dev"
    exit 1
fi

echo
echo "Ready. Next: ./build.sh && ./build/gygax doctor"
