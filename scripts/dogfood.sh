#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"

FAST=0
LINT=0
PACKAGE=0
for arg in "$@"; do
    case "$arg" in
        --fast) FAST=1 ;;
        --lint) LINT=1 ;;
        --package) PACKAGE=1 ;;
        *) echo "usage: $0 [--fast] [--lint] [--package]" >&2; exit 2 ;;
    esac
done

CXX="${CXX:-clang++-18}"
RESULTS=()
FAILED=0

step() {
    local name="$1"; shift
    echo
    echo "=== $name"
    if "$@"; then
        RESULTS+=("PASS  $name")
    else
        RESULTS+=("FAIL  $name")
        FAILED=1
    fi
}

configure_build() {
    local dir="$1"; shift
    cmake -G Ninja -B "$dir" -S . -DCMAKE_CXX_COMPILER="$CXX" "$@" >/dev/null && cmake --build "$dir"
}

run_ctest() { ctest --test-dir "$1" --output-on-failure -j"$(nproc)"; }

daemon_smoke() {
    local dir="$1"
    local port token log pid
    port=$((20000 + RANDOM % 20000))
    token="$(head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n')"
    log="$(mktemp)"
    printf '%s\n' "$token" > "$log.token"
    "$dir/gygax" serve --port "$port" --token-file "$log.token" >"$log" 2>&1 &
    pid=$!
    local ok=0
    for _ in $(seq 1 50); do
        if curl -fsS "http://127.0.0.1:$port/readyz" >/dev/null 2>&1; then ok=1; break; fi
        sleep 0.1
    done
    if [ "$ok" -ne 1 ]; then echo "daemon did not become ready"; cat "$log"; kill "$pid" 2>/dev/null; return 1; fi

    local base="http://127.0.0.1:$port" auth="Authorization: Bearer $token" rc=0
    [ "$(curl -s -o /dev/null -w '%{http_code}' "$base/v1/models")" = "401" ] || { echo "unauthenticated request was not rejected"; rc=1; }
    curl -fsS -H "$auth" "$base/v1/models" | grep -q '"gygax-agent"' || { echo "models missing"; rc=1; }
    curl -fsS -H "$auth" -H 'Content-Type: application/json' \
        -d '{"model":"echo","messages":[{"role":"user","content":"dogfood"}]}' \
        "$base/v1/chat/completions" | grep -q 'echo: dogfood' || { echo "chat failed"; rc=1; }
    curl -fsSN -H "$auth" -d '{"model":"echo","stream":true,"messages":[{"role":"user","content":"s"}]}' \
        "$base/v1/chat/completions" | grep -q 'data: \[DONE\]' || { echo "stream failed"; rc=1; }
    curl -fsS -H "$auth" -d '{"model":"gygax-agent:echo","messages":[{"role":"user","content":"!tool math.eval 6*7"}]}' \
        "$base/v1/chat/completions" | grep -q '"content":"42"' || { echo "agent failed"; rc=1; }
    curl -fsS -H "$auth" "$base/metrics" | grep -q 'gygax_agent_runs_total{result="completed"} 1' || { echo "metrics missing agent run"; rc=1; }
    curl -fsS -H "$auth" -d '{"line":"+8 ba99x drone=quadcopter power=solar"}' "$base/v1/logistics/events" >/dev/null || { echo "logistics record failed"; rc=1; }
    curl -fsS -H "$auth" -d '{"line":"-3 ba99x drone=quadcopter power=solar"}' "$base/v1/logistics/events" | grep -q '"line":"-3 ba99x' || { echo "logistics loss failed"; rc=1; }
    curl -fsS -H "$auth" "$base/v1/logistics/summary?since=1d" | grep -q '"active":5' || { echo "logistics summary wrong"; rc=1; }
    [ "$(curl -s -o /dev/null -w '%{http_code}' -H "$auth" -d '{"command":"arm"}' "$base/v1/devices/none/command")" = "403" ] || { echo "device commands were not disabled"; rc=1; }
    "$dir/gygax" doctor --url "$base" --token "$token" --full || rc=1

    kill -TERM "$pid"
    local waited=0
    while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt 50 ]; do sleep 0.1; waited=$((waited + 1)); done
    if kill -0 "$pid" 2>/dev/null; then echo "daemon ignored SIGTERM"; kill -KILL "$pid"; rc=1; fi
    wait "$pid" 2>/dev/null
    grep -q 'shutting down' "$log" || { echo "no graceful shutdown log"; rc=1; }
    rm -f "$log" "$log.token"
    return "$rc"
}

python_tests() {
    GYGAX_LIB="$1/libgygax_c.so" GYGAX_BIN="$1/gygax" GYGAX_PLUGIN="$1/geo_plugin.so" PYTHONPATH="$ROOT/python" python3 -m unittest discover -s "$ROOT/python/tests" -v
}

toy_smoke() {
    "$1/gygax_asm" "$ROOT/examples/coco/hello.asm" -o "$1/hello.dsk" --cpu 6809 && "$1/gygax_coco" "$1/hello.dsk" --cpu 6809 --quiet | tee /dev/stderr | grep -q '1 of 4096 pixels set'
}

package_check() {
    local dir="$1" stage
    stage="$(mktemp -d)"
    cmake --build "$dir" >/dev/null || return 1
    DESTDIR="$stage" cmake --install "$dir" --prefix /usr >/dev/null || return 1
    LD_LIBRARY_PATH="$stage/usr/lib" "$stage/usr/bin/gygax" version || return 1
    if command -v dpkg-deb >/dev/null 2>&1; then
        (cd "$dir" && cpack -G DEB >/dev/null) || return 1
        dpkg-deb -I "$dir"/gygax_*.deb | grep -E 'Package|Version|Depends' || return 1
    fi
    rm -rf "$stage"
}

step "clean build (RelWithDebInfo, -Werror)" bash -c "$(declare -f configure_build); CXX='$CXX'; rm -rf build/dogfood; configure_build build/dogfood"
step "unit and integration tests" run_ctest build/dogfood
step "live daemon smoke test" daemon_smoke build/dogfood
step "python bindings" python_tests build/dogfood
step "retro toolchain (assemble and emulate)" toy_smoke build/dogfood

if [ "$FAST" -eq 0 ]; then
    step "AddressSanitizer + UBSan tests" bash -c "$(declare -f configure_build run_ctest); CXX='$CXX'; configure_build build/dogfood-asan -DCMAKE_BUILD_TYPE=Debug -DGYGAX_SANITIZE=address,undefined && run_ctest build/dogfood-asan"
    step "ThreadSanitizer tests" bash -c "$(declare -f configure_build run_ctest); CXX='$CXX'; configure_build build/dogfood-tsan -DCMAKE_BUILD_TYPE=Debug -DGYGAX_SANITIZE=thread && run_ctest build/dogfood-tsan"
fi
[ "$LINT" -eq 1 ] && step "lint" env BUILD_DIR="$ROOT/build/dogfood" "$ROOT/scripts/lint.sh" all
[ "$PACKAGE" -eq 1 ] && step "install and package" package_check build/dogfood

echo
echo "=== summary"
printf '%s\n' "${RESULTS[@]}"
exit "$FAILED"
