# Contributing

## Build and test

```bash
./setup.sh
./build.sh test                     # configure, build with -Werror, run all tests
./build/gygax doctor                # in-process self test of the whole stack
```

Run one test: `./build/gygax_tests --gtest_filter='EnginePool.*'` or `ctest --test-dir build -R EnginePool`.

## Before you open a pull request

```bash
./scripts/lint.sh                   # clang-format, clang-tidy, cppcheck
./scripts/dogfood.sh --fast         # clean build, tests, live daemon smoke test, Python, toolchain
./scripts/dogfood.sh                # adds AddressSanitizer, UBSan and ThreadSanitizer runs
```

CI runs the same checks. Changes to concurrency, networking or the C ABI should have been through the sanitizer run.

## Conventions

- C++26, clang 18+. Module interfaces (`*.cppm`) for the stateful runtime; plain headers plus `.cpp` for everything else. New code should default to plain headers unless it needs the runtime modules.
- No comments in source files. Names, types and tests carry the intent; put explanations in `docs/`.
- No `void*` casts outside ABI boundaries, no raw owning pointers, no unchecked optionals. Warnings are errors (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`).
- Do not print from library code; use `gygax::log`. Only examples and the CLI write to stdout.
- New behavior needs a test that runs without hardware or network access beyond loopback. Device code is tested against pseudo-terminals, fake sysfs trees and local sockets.
- Everything user-visible must be documented in `docs/` or `README.md` and listed in `CHANGELOG.md`.
- Code that is unused or cannot be verified belongs in `trash/` with a note in `trash/README.md`, not in the build.

## Commit messages

Short imperative subject; a body explaining why when it is not obvious.
