# Windows 11

Gygax's C++ core is POSIX-only by design (`CMakeLists.txt` refuses to configure on `WIN32`) and stays that way: it uses `pthread`/`std::jthread` semantics, BSD sockets, `dlopen` for plugins, `fork`/`exec` for MCP servers, `poll`, PTYs in tests, and raw SocketCAN `ioctl`s that have no Windows equivalent. Porting all of that to Win32 is a large, invasive project (and SocketCAN specifically cannot be ported at all, because it is a Linux kernel feature); it is not attempted here, and `include/gygax/core/posix.hpp` and friends assume it never will be.

What Windows 11 gets instead is a real, working pipeline through **Docker Desktop with the WSL2 backend** (the Windows 11 default), which is genuinely built and tested, not just documented:

- A `dev` stage in the root `Dockerfile` (`docker build --target dev`) carries the full Linux toolchain (clang-18, clang-tidy, clang-format, cppcheck, cmake, ninja, python3+numpy) but no copy of the source, so a checkout is bind-mounted into it at `docker run` time. Editing files on the Windows side is immediately visible inside the container. There is no copy/sync step and no stale image to rebuild by hand.
- Every `.sh` at the repo root and under `scripts/` has a `.ps1` (the real script) and a `.bat` (a one-line `cmd.exe` shim that calls the `.ps1`) counterpart. Each just runs the *same, unmodified* `.sh` inside the `dev` image via `scripts\DevContainer.ps1`, so there is exactly one implementation of every build/test/lint step to maintain, and Windows behavior can never drift from Linux CI behavior.

| Linux/macOS | Windows 11 | What it does |
| --- | --- | --- |
| `./setup.sh` | `.\setup.ps1` / `.bat` | Checks Docker Desktop + WSL2 are present and working, then builds `gygax-dev` |
| `./build.sh [build\|test\|install\|configure\|clean]` | `.\build.ps1 ...` / `.bat` | Runs `build.sh` in the container |
| `./assemble.sh ...` | `.\assemble.ps1 ...` / `.bat` | Runs `assemble.sh` in the container (`-it` when `--debug` is passed) |
| `./scripts/dogfood.sh [--fast]` | `.\scripts\dogfood.ps1 ...` / `.bat` | Runs `dogfood.sh` in the container |
| `./scripts/lint.sh [all\|format\|tidy\|cppcheck]` | `.\scripts\lint.ps1 ...` / `.bat` | Runs `lint.sh` in the container |
| `./scripts/sdk-check.sh DIR BIN` | `.\scripts\sdk-check.ps1 ...` / `.bat` | Runs `sdk-check.sh` in the container |
| `./scripts/collect-diagnostics.sh` | `.\scripts\collect-diagnostics.ps1` / `.bat` | Runs `collect-diagnostics.sh` in the container |
| `./scripts/docker-build.sh` | `.\scripts\docker-build.ps1` / `.bat` | Builds both the `gygax` (production) and `gygax-dev` images |
| `./scripts/devshell.sh` | *(use `docker run -it ... gygax-dev bash` directly, or just `.\build.ps1` etc.)* | Interactive shell in the dev image |

```powershell
.\setup.ps1                 # one-time: checks Docker, builds gygax-dev
.\build.ps1 test             # cmake configure + build + ctest, in the container
.\scripts\lint.ps1 all
.\scripts\dogfood.ps1 --fast
```

`BUILD_DIR`/`BUILD_TYPE` environment variables are forwarded the same way they are read on Linux (`$env:BUILD_DIR = "build/win"`). Build output lands under the checkout on the Windows side, because the container only ever writes into the bind-mounted `/src`; nothing is left behind in the image.

## Running the service

The production image (`docker build -t gygax .`, or `.\scripts\docker-build.ps1`) is the same multi-stage build the Linux CI "package" job and `Dockerfile` have always produced. No Windows-specific image exists or is needed, since it never runs anything Windows-specific:

```powershell
docker run --rm -p 1984:1984 -e GYGAX_API_TOKEN=... gygax
curl http://127.0.0.1:1984/healthz
```

To reach a Gygax service that is itself running as a container from *inside* another container (e.g. `scripts\collect-diagnostics.ps1` pointed at a local instance), use Docker Desktop's `host.docker.internal` DNS name instead of `127.0.0.1`, because container loopback addresses do not cross container boundaries the way they do processes on the same host.

## Native WSL2, no Docker

If you would rather not go through Docker at all: install Ubuntu from the Microsoft Store (`wsl --install -d Ubuntu`), open an Ubuntu shell, and follow the normal Linux instructions in the top-level `README.md` (`./setup.sh && ./build.sh test`) directly inside it. Nothing there is Windows-specific; WSL2 is a real Linux kernel. This is what the Docker `dev` image effectively automates for you, minus needing to install the toolchain yourself.

## What is not supported

- Native compilation with MSVC or clang-cl. `WIN32` is rejected at CMake configure time on purpose (see `CMakeLists.txt`); this is a statement of scope, not a bug to file.
- SocketCAN (`bus::SocketCanBus`) has no meaning outside Linux and will not gain one; `LoopbackCanNetwork` (`virtual:name` interfaces) works everywhere the tests run, including in the `dev` container.
- GUI tooling. There is none yet (see `docs/GYDE.md` for the design of one); everything here is the CLI/HTTP service plus the scripts above.
