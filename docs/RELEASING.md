# Releasing Gygax

Pushing a version tag matching the CMake project version runs
`.github/workflows/release.yml`. For example, if `project(gygax VERSION 0.2.1)`
is set in `CMakeLists.txt` and `GYGAX_VERSION_PRERELEASE` is empty, push `v0.2.1`.
With `GYGAX_VERSION_PRERELEASE` set to `alpha.1`, push `v0.2.1-alpha.1`. The label must be
`alpha.N`, `beta.N` or `rc.N`; CMake refuses anything else.

The workflow runs two jobs in parallel:

- **Source archive:** creates a deterministic `.tar.gz` from the exact tagged
  commit with `git archive`. GitHub's generated source archives remain available
  as well.
- **Linux SDK:** configures and builds the Release preset, runs the full CTest
  suite (including the installed, out-of-tree plugin SDK consumer), and creates
  the Debian package and relocatable Linux tarball with CPack. It validates that
  both packages can be inspected before publishing.

After both jobs succeed, the publish job creates or updates the GitHub Release
with the source archive, Linux SDK packages, and a `SHA256SUMS` file covering
the release assets.

Before tagging:

1. Update `project(... VERSION ...)`, `GYGAX_VERSION_PRERELEASE` and `GYGAX_RELEASE_NAME`
   in `CMakeLists.txt`, the Python package version in its PEP 440 form
   (`0.2.1-alpha.1` is `0.2.1a1`) in `python/pyproject.toml` and
   `python/gygax/__init__.py` (the Python tests check it), `CHANGELOG.md`, and `docs/releases/<tag>.md`.
2. Ensure the normal CI checks pass on the exact release commit.
3. Create and push an annotated version tag, for example:

   ```bash
   git tag -a v0.2.1 -m "Gygax 0.2.1 \"Name\""
   git push origin v0.2.1
   ```

A pre-release is tagged with a suffix, for example `v0.2.1-alpha.1`: the whole
tag must match the CMake version and pre-release label, and the release is
marked as a pre-release. Pre-release packages are named with the full version
(`gygax_0.2.1-alpha.1_amd64.deb`); inside, the Debian version is
`0.2.1~alpha.1`, which sorts before the final `0.2.1`. If `docs/releases/<tag>.md` exists, it becomes the release notes
and its first `# ` heading the title; otherwise GitHub generates notes.

Tags whose version does not match the CMake project version fail before
assets are published. The current distributable SDK targets Linux x86-64
(Debian/Ubuntu `.deb` and relocatable Linux `.tar.gz`); source is provided for
other supported build environments. macOS remains best-effort in CI and is not
published as a prebuilt binary.
