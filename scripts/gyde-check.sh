#!/usr/bin/env bash
# Hardware-free selfcheck for gyde, the terminal preview of the Gygax IDE
# (docs/GYDE.md). Starts a real service and drives gyde's actual interactive
# loop over a pipe, exactly as a person typing at it would, then checks the
# output. Registered as the gyde_selfcheck ctest entry.
set -euo pipefail

gygax_bin=${1:?path to the gygax binary}
gyde_bin=${2:?path to the gyde binary}

port=$((20000 + RANDOM % 20000))
token="gyde-selfcheck-$RANDOM"
log="$(mktemp)"
trap 'kill "$server_pid" 2>/dev/null || true; rm -f "$log"' EXIT

"$gygax_bin" serve --port "$port" --token "$token" --engine echo --log-level error >"$log" 2>&1 &
server_pid=$!

for _ in $(seq 1 50); do
    curl -fsS "http://127.0.0.1:$port/healthz" >/dev/null 2>&1 && break
    sleep 0.1
done

out="$(printf 'status\nengines\nask hi\ntool math.eval 6*7\ntool no.such.tool x\nquit\n' | "$gyde_bin" --url "http://127.0.0.1:$port" --token "$token")"

fail() { echo "gyde_selfcheck: $1" >&2; echo "--- gyde output ---" >&2; echo "$out" >&2; exit 1; }

echo "$out" | grep -q '\[up\] engines' || fail "the flash never showed the service as up"
echo "$out" | grep -q '"healthy": true' || fail "status did not report a healthy node"
echo "$out" | grep -q 'echo: hi' || fail "ask did not get the echo backend's reply"
echo "$out" | grep -q '"output":"42"' || fail "math.eval tool call did not return 42"
echo "$out" | grep -q 'not_found' || fail "an unknown tool should have reported not_found"

echo "gyde_selfcheck: ok"
