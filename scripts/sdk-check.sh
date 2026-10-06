#!/usr/bin/env bash
set -euo pipefail

build_dir=${1:?build directory}
gygax_bin=${2:?gygax binary}
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
server_pid=""

cleanup() {
    if [ -n "$server_pid" ]; then kill "$server_pid" 2>/dev/null || true; fi
    rm -rf "$work"
}
trap cleanup EXIT

cmake --install "$build_dir" --prefix "$work/prefix" >/dev/null
cmake -S "$root/examples/plugin-consumer" -B "$work/consumer" -G Ninja -DCMAKE_PREFIX_PATH="$work/prefix" \
    -DCMAKE_CXX_COMPILER="${CXX:-clang++-18}" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$work/consumer" >/dev/null
plugin=$(find "$work/consumer" -name fleet_plugin.so | head -n 1)
test -f "$plugin"

port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
"$gygax_bin" serve --port "$port" --log-level error --plugin "$plugin" &
server_pid=$!
for _ in $(seq 1 100); do
    if curl -fs "http://127.0.0.1:$port/healthz" >/dev/null 2>&1; then break; fi
    sleep 0.1
done

reply=$(curl -fs -X POST "http://127.0.0.1:$port/v1/tools/plugin.fleet.sorties_needed/invoke" \
    -H 'Content-Type: application/json' -d '{"input":"{\"items\":25,\"capacity\":8}"}')
case "$reply" in
    *'"output":"4"'*) echo "sdk consumer plugin: ok" ;;
    *) echo "unexpected reply: $reply" >&2; exit 1 ;;
esac
