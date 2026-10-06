#!/usr/bin/env bash
set -euo pipefail

URL="${GYGAX_URL:-http://127.0.0.1:1984}"
TOKEN="${GYGAX_API_TOKEN:-}"
BIN="${GYGAX_BIN:-gygax}"
OUT="${1:-gygax-diagnostics-$(date -u +%Y%m%dT%H%M%SZ).tar.gz}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/bundle"
B="$WORK/bundle"

redact() {
    local hostname_re
    hostname_re="$(hostname 2>/dev/null || echo __nohost__)"
    sed -E \
        -e "s/${TOKEN:-__notoken__}/<token>/g" \
        -e "s/(Bearer )[A-Za-z0-9._~+\/=-]+/\1<token>/g" \
        -e 's/([0-9]{1,3}\.){3}[0-9]{1,3}/<ip>/g' \
        -e "s/${hostname_re}/<host>/g" \
        -e 's#/home/[^/ ]+#/home/<user>#g'
}

{
    echo "collected: $(date -u +%FT%TZ)"
    "$BIN" version 2>&1 || true
    uname -a
    [ -f /etc/os-release ] && cat /etc/os-release
    echo "cpus: $(nproc 2>/dev/null || echo unknown)"
    grep -E 'MemTotal|MemAvailable' /proc/meminfo 2>/dev/null || true
} 2>&1 | redact > "$B/system.txt"

env | grep -E '^GYGAX_' | sed -E 's/^(GYGAX_(API_TOKEN|PEER_TOKEN|ENGINE_API_KEY))=.*/\1=<set>/' | redact > "$B/environment.txt" || true

"$BIN" doctor --json 2>&1 | redact > "$B/doctor.json" || true

if command -v curl >/dev/null 2>&1; then
    auth=()
    [ -n "$TOKEN" ] && auth=(-H "Authorization: Bearer $TOKEN")
    for path in healthz readyz version v1/node v1/engines v1/cluster v1/tools metrics; do
        curl -fsS --max-time 5 "${auth[@]}" "$URL/$path" 2>&1 | redact > "$B/$(echo "$path" | tr '/' '_').txt" || echo "unavailable" > "$B/$(echo "$path" | tr '/' '_').txt"
    done
fi

if command -v journalctl >/dev/null 2>&1; then
    journalctl -u gygax --no-pager -n 1000 2>/dev/null | redact > "$B/journal.txt" || true
fi

tar -C "$WORK" -czf "$OUT" bundle
echo "wrote $OUT (tokens, IP addresses, host and user names redacted; review before sharing)"
