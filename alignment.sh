#!/usr/bin/env bash
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ALIGNMENT_BUILD_DIR:-$ROOT/build/alignment}"
REPORT="$BUILD_DIR/alignment-report.log"
CXX="${CXX:-clang++-18}"
CC="${CC:-clang-18}"

usage() {
    cat <<'EOF'
Usage: ./alignment.sh

Builds every configured project target with C/C++ cast-alignment warnings and
the alignment undefined-behavior sanitizer, then runs the complete CTest suite.

Options:
  -h, --help        Show this help.

Environment:
  CC, CXX               C and C++ compilers (defaults: clang-18, clang++-18)
  ALIGNMENT_BUILD_DIR   Isolated build directory (default: build/alignment)
EOF
}

for arg in "$@"; do
    case "$arg" in
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; usage >&2; exit 2 ;;
    esac
done

fail() {
    echo "alignment.sh: $*" >&2
    exit 2
}

command -v cmake >/dev/null 2>&1 || fail "cmake is required"
command -v ctest >/dev/null 2>&1 || fail "ctest is required"
command -v ninja >/dev/null 2>&1 || fail "ninja is required"
command -v "$CC" >/dev/null 2>&1 || fail "C compiler not found: $CC"
command -v "$CXX" >/dev/null 2>&1 || fail "C++ compiler not found: $CXX"

mkdir -p "$BUILD_DIR" || fail "cannot create build directory: $BUILD_DIR"
: > "$REPORT" || fail "cannot write report: $REPORT"

inventory_sources() {
    git -C "$ROOT" ls-files -co --exclude-standard |
        grep -E '\.(c|cc|cpp|cxx|h|hh|hpp|hxx|cppm|ipp|tpp)$' || true
}

check_public_headers() {
    local rc=0 header relative
    local include_flags=(-I"$ROOT/include" -I"$BUILD_DIR/generated" -I"$ROOT/src/gygax/robotics" -isystem /usr/include/eigen3)

    echo "=== Compile each public C++ header independently" | tee -a "$REPORT"
    while IFS= read -r -d '' header; do
        relative="${header#"$ROOT/include/"}"
        if ! printf '#include <%s>\n' "$relative" |
            "$CXX" -std=c++26 -Wcast-align -fsanitize=alignment "${include_flags[@]}" -x c++ -fsyntax-only - 2>&1 |
            tee -a "$REPORT"; then
            echo "Failed standalone C++ header: $relative" | tee -a "$REPORT"
            rc=1
        fi
    done < <(find "$ROOT/include" -type f -name '*.hpp' -print0 | sort -z)

    echo "=== Compile each public C header independently" | tee -a "$REPORT"
    while IFS= read -r -d '' header; do
        relative="${header#"$ROOT/include/"}"
        if ! printf '#include <%s>\n' "$relative" |
            "$CC" -std=c17 -Wcast-align -fsanitize=alignment -I"$ROOT/include" -x c -fsyntax-only - 2>&1 |
            tee -a "$REPORT"; then
            echo "Failed standalone C header: $relative" | tee -a "$REPORT"
            rc=1
        fi
    done < <(find "$ROOT/include" -type f -name '*.h' -print0 | sort -z)

    return "$rc"
}

run_checks() {
    local rc=0
    local inventory="$BUILD_DIR/alignment-sources.txt"

    inventory_sources > "$inventory"
    {
        echo "=== Alignment analysis: $(date -Is)"
        echo "Root: $ROOT"
        echo "C compiler: $CC"
        echo "C++ compiler: $CXX"
        echo "Build directory: $BUILD_DIR"
        echo "Project C/C++ source/header inventory: $(wc -l < "$inventory") files"
        echo "Public C++ headers: $(find "$ROOT/include" -type f -name '*.hpp' | wc -l)"
        echo "Public C headers: $(find "$ROOT/include" -type f -name '*.h' | wc -l)"
        echo "Inventory: $inventory"
        echo
        echo "The build instruments all configured targets; public headers are syntax-checked independently."
        echo "Runtime alignment checks cover the complete CTest suite."
        echo
    } | tee -a "$REPORT"

    echo "=== Configure with -Wcast-align and -fsanitize=alignment" | tee -a "$REPORT"
    if ! cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_C_COMPILER="$CC" \
        -DCMAKE_CXX_COMPILER="$CXX" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_FLAGS=-Wcast-align \
        -DCMAKE_CXX_FLAGS=-Wcast-align \
        -DGYGAX_SANITIZE=alignment \
        -DGYGAX_BUILD_TESTS=ON \
        -DGYGAX_BUILD_EXAMPLES=ON \
        -DGYGAX_BUILD_TOYS=ON \
        -DGYGAX_WERROR=ON 2>&1 | tee -a "$REPORT"; then
        rc=1
    else
        echo "=== Build all project targets" | tee -a "$REPORT"
        if ! cmake --build "$BUILD_DIR" --parallel "$(getconf _NPROCESSORS_ONLN)" 2>&1 | tee -a "$REPORT"; then
            rc=1
        else
            if ! check_public_headers; then
                rc=1
            fi
            echo "=== Run complete test suite with alignment sanitizer fatal" | tee -a "$REPORT"
            if ! UBSAN_OPTIONS="${UBSAN_OPTIONS:+$UBSAN_OPTIONS:}halt_on_error=1:print_stacktrace=1" \
                ctest --test-dir "$BUILD_DIR" --output-on-failure --parallel "$(getconf _NPROCESSORS_ONLN)" 2>&1 |
                tee -a "$REPORT"; then rc=1; fi
        fi
    fi

    if [ "$rc" -eq 0 ]; then
        echo "=== Alignment checks passed" | tee -a "$REPORT"
    else
        echo "=== Alignment checks failed; see $REPORT" | tee -a "$REPORT"
    fi
    return "$rc"
}

if run_checks; then
    exit 0
fi

echo "Checks failed. Review $REPORT." >&2
exit 1
