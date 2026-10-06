#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

BUILD_DIR="${BUILD_DIR:-build}"
ASM_BIN="$BUILD_DIR/gygax_asm"
EMU_BIN="$BUILD_DIR/gygax_coco"

usage() {
    echo "usage: $0 <input.asm> -o <output.dsk> [--cpu 6809|6502] [--no-run] [--debug] [--ai] [--poke ADDR=VAL]... [--patch FILE]"
}

INPUT=""
OUTPUT="out.dsk"
CPU="6809"
RUN=1
EMU_ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        -o) OUTPUT="$2"; shift 2 ;;
        --cpu) CPU="$2"; shift 2 ;;
        --no-run) RUN=0; shift ;;
        --debug) EMU_ARGS+=(--debug); shift ;;
        --ai) EMU_ARGS+=(--ai); shift ;;
        --quiet) EMU_ARGS+=(--quiet); shift ;;
        --poke) EMU_ARGS+=(--poke "$2"); shift 2 ;;
        --patch) EMU_ARGS+=(--patch "$2"); shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) INPUT="$1"; shift ;;
    esac
done

if [ -z "$INPUT" ]; then
    usage
    exit 2
fi

if [ ! -x "$ASM_BIN" ] || [ ! -x "$EMU_BIN" ]; then
    ./build.sh
fi

"$ASM_BIN" "$INPUT" -o "$OUTPUT" --cpu "$CPU"

if [ "$RUN" -eq 1 ]; then
    "$EMU_BIN" "$OUTPUT" --cpu "$CPU" ${EMU_ARGS[@]+"${EMU_ARGS[@]}"}
fi
