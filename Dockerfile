FROM ubuntu:24.04 AS toolchain
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        clang-18 clang-tools-18 clang-format-18 clang-tidy-18 cppcheck cmake ninja-build \
        ca-certificates git python3 python3-numpy curl libeigen3-dev libgmp-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src

FROM toolchain AS build
COPY . .
RUN cmake -G Ninja -B /build -S . -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_BUILD_TYPE=Release \
        -DGYGAX_BUILD_TESTS=OFF -DGYGAX_BUILD_EXAMPLES=OFF -DGYGAX_BUILD_TOYS=OFF \
    && cmake --build /build --target gygax_cli gygax_c gyde \
    && DESTDIR=/stage cmake --install /build --prefix /usr

# `dev` is not a production image: it carries the full toolchain (clang, clang-tidy,
# cppcheck, cmake, ninja, python3+numpy) with no copy of the source, so a repo checkout
# can be bind-mounted at `docker run` time for a live edit/build/test loop. This is what
# build.ps1/build.bat, setup.ps1, scripts/dogfood.ps1 and scripts/lint.ps1 run under on
# Windows (see docs/WINDOWS.md); it also works on Linux/macOS as ./scripts/devshell.sh.
FROM toolchain AS dev
RUN git config --system --add safe.directory '*'
CMD ["bash"]

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 libgmp10 libgmpxx4ldbl ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --no-create-home --uid 10001 --shell /usr/sbin/nologin gygax
COPY --from=build /stage/usr/ /usr/
ENV GYGAX_HOST=0.0.0.0 GYGAX_PORT=1984 GYGAX_LOG_FORMAT=json
USER gygax
EXPOSE 1984
HEALTHCHECK --interval=15s --timeout=3s --start-period=5s --retries=3 \
    CMD curl -fsS http://127.0.0.1:1984/healthz || exit 1
ENTRYPOINT ["/usr/bin/gygax"]
CMD ["serve"]
