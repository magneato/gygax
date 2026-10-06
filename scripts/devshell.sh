#!/usr/bin/env bash
# Drops into a shell in the gygax-dev Docker image with this checkout
# bind-mounted at /src, for developing without installing the toolchain
# locally. This is what build.ps1/dogfood.ps1/lint.ps1 do non-interactively
# on Windows (see docs/WINDOWS.md); on Linux/macOS you can just install the
# toolchain (./setup.sh) instead, but this works too, e.g. on a machine
# without root.
set -euo pipefail
cd "$(dirname "$0")/.."

docker build --target dev -t gygax-dev .
exec docker run --rm -it -v "$PWD:/src" -w /src -p 1984:1984 gygax-dev "${@:-bash}"
