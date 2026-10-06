#!/usr/bin/env bash
# Builds every Docker image this repo defines: the production `gygax` image
# (the default, last stage) and the `gygax-dev` toolchain image used by the
# Windows wrapper scripts (build.ps1, scripts/dogfood.ps1, ...) and by
# scripts/devshell.sh on Linux/macOS. Works identically on Linux, macOS and
# Windows (docker-build.ps1/.bat) since it only drives the Docker CLI.
set -euo pipefail
cd "$(dirname "$0")/.."

TAG="${GYGAX_IMAGE:-gygax}"
DEV_TAG="${GYGAX_DEV_IMAGE:-gygax-dev}"

echo "=== building $TAG (production)"
docker build -t "$TAG" .

echo
echo "=== building $DEV_TAG (toolchain, for devshell/CI-style builds)"
docker build --target dev -t "$DEV_TAG" .

echo
echo "built: $TAG, $DEV_TAG"
echo "run the service:  docker run --rm -p 1984:1984 -e GYGAX_API_TOKEN=... $TAG"
echo "dev shell:        ./scripts/devshell.sh   (or build.ps1/dogfood.ps1/... on Windows)"
