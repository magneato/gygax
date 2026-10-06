#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build/dev}"
FORMAT="${CLANG_FORMAT:-clang-format-18}"
TIDY="${CLANG_TIDY:-clang-tidy-18}"
MODE="${1:-all}"

cd "$ROOT"

sources() {
    git ls-files -co --exclude-standard -- 'include/*' 'src/*' 'tools/*' 'examples/*' 'tests/*' |
        grep -E '\.(cpp|cppm|hpp|h)$'
}

plain_units() {
    sources | grep -E '\.cpp$' | grep -vE '^(examples/|tests/)' | grep -vE '^(src/gygax/service/service\.cpp|src/gygax/core/base\.cpp|tools/gygax_coco\.cpp)$'
}

run_format() {
    echo "== clang-format"
    sources | xargs "$FORMAT" --dry-run --Werror
}

run_tidy() {
    echo "== clang-tidy"
    if [ ! -f "$BUILD/compile_commands.json" ]; then
        echo "compile_commands.json not found in $BUILD; configure first" >&2
        exit 2
    fi
    plain_units | xargs -P "$(nproc)" -n 4 "$TIDY" -p "$BUILD" --quiet --warnings-as-errors='*'
}

run_cppcheck() {
    echo "== cppcheck"
    if command -v cppcheck >/dev/null 2>&1; then
        cppcheck --quiet --error-exitcode=1 --enable=warning,performance,portability \
            --suppress=missingIncludeSystem --suppress=unmatchedSuppression --inline-suppr \
            -I include -I "$BUILD/generated" $(plain_units)
    else
        echo "cppcheck not installed; skipping"
    fi
}

case "$MODE" in
    format) run_format ;;
    tidy) run_tidy ;;
    cppcheck) run_cppcheck ;;
    all) run_format; run_tidy; run_cppcheck ;;
    *) echo "usage: $0 [format|tidy|cppcheck|all]" >&2; exit 2 ;;
esac
echo "lint ok"
