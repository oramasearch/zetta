# SPDX-License-Identifier: MIT
#
# Orama Zetta: headless Linux/amd64 image.
#
# Builds the single `zetta` binary (headless core + the SDL3/ImGui headed UI; there is no
# headless-only build target yet) from this source tree, and runs it with `--headless`.
# The build context is the repository root, filtered by `.dockerignore`:
#
#   git clone --recurse-submodules https://github.com/oramasearch/zetta.git && cd zetta
#   docker buildx build --platform linux/amd64 -t <registry>/zetta:<version> .
#
# The submodules must already be checked out (the build runs no git). The runtime never
# initializes SDL (headless), so the runtime stage needs no X11/GL: only OpenSSL 3, libpq and the
# C++ runtime. See docs/build.md, "Container image".

# ---------- build stage ----------
# Pinned by digest: a moved tag must never change the image.
FROM ubuntu:24.04@sha256:2198e4c65076e9cb0b3d695219df667c0e3b0072da64ca3c2648aca17dcecaf5 AS build
ENV DEBIAN_FRONTEND=noninteractive

# Clang 17+ is required for C++26; install clang-19 from the official LLVM apt channel. Plus the
# documented Ubuntu build deps (scripts/install-deps.sh), including SDL3's X11/Wayland/GL/audio
# headers, which the shipped binary links even though headless never uses them. OpenSSL 3 is
# required (inter-node mTLS + cert minting); libpq-dev is for the Postgres source connector.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates wget gnupg lsb-release software-properties-common \
      build-essential cmake ninja-build git ccache lld \
      zlib1g-dev libssl-dev \
      libpq-dev \
      libx11-dev libxext-dev libxrandr-dev libxi-dev libxcursor-dev \
      libxfixes-dev libxkbcommon-dev libwayland-dev wayland-protocols \
      libgl1-mesa-dev libegl1-mesa-dev libdbus-1-dev libudev-dev \
      libpipewire-0.3-dev libpulse-dev libasound2-dev \
 && wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh && chmod +x /tmp/llvm.sh && /tmp/llvm.sh 19 \
 && apt-get install -y --no-install-recommends libc++-19-dev libc++abi-19-dev \
 && rm -rf /var/lib/apt/lists/*

# Ubuntu 24.04 ships CMake 3.28, which does NOT know cxx_std_26 for Clang 19
# (target_compile_features rejects it). Install a newer CMake that supports C++26.
ARG CMAKE_VERSION=3.31.5
RUN wget -qO /tmp/cmake.sh "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.sh" \
 && sh /tmp/cmake.sh --skip-license --prefix=/usr/local && rm -f /tmp/cmake.sh && cmake --version

WORKDIR /src
COPY . /src

# CPU-only (CUDA off), tests off (skips the GoogleTest fetch + the test targets). FetchContent
# pulls SDL3, ImGui, json-schema-validator and Boost into build/_deps at configure time (needs
# network). Ninja uses every core.
RUN cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DZETTA_BUILD_TESTS=OFF \
      -DZETTA_ENABLE_CUDA=OFF \
      -DCMAKE_C_COMPILER=clang-19 \
      -DCMAKE_CXX_COMPILER=clang++-19 \
 && cmake --build build --target zetta -j "$(nproc)" \
 && { strip build/bin/zetta || true; } \
 && mkdir -p /src/webapp

# ---------- runtime stage ----------
FROM ubuntu:24.04@sha256:2198e4c65076e9cb0b3d695219df667c0e3b0072da64ca3c2648aca17dcecaf5 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
# Runtime libs only. (If `ldd` shows libc++, add libc++1-19/libc++abi1-19 from the LLVM channel;
# this assumes the default libstdc++ toolchain.)
RUN apt-get update && apt-get install -y --no-install-recommends \
      libssl3 libstdc++6 libpq5 ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd -m -u 10001 -s /usr/sbin/nologin zetta \
 && mkdir -p /data && chown zetta:zetta /data

COPY --from=build /src/build/bin/zetta /usr/local/bin/zetta

# The Studio web app bundle (webapp/), seated via ZETTA_WEBAPP_DIR (an explicit --webapp-dir
# wins). The node serves it at /studio/; its only data path is /v1. A tree without the bundle
# (the public export leaves it out until it is rebuilt from source) gets an empty directory:
# with no index.html the node serves its built-in server-rendered UI at / instead.
COPY --from=build --chown=zetta:zetta /src/webapp /opt/zetta/webapp
ENV ZETTA_WEBAPP_DIR=/opt/zetta/webapp

USER zetta
WORKDIR /data
VOLUME ["/data"]
# 7777 = peer replication + signal (mTLS); 4800 = MCP + REST + serving (one HTTP port).
EXPOSE 7777 4800
ENTRYPOINT ["zetta"]
CMD ["--headless", "--data-dir", "/data", "--listen", "0.0.0.0:7777", "--mcp-listen", "0.0.0.0:4800"]
