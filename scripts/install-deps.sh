#!/usr/bin/env bash
#
# install-deps.sh — install zetta's system build dependencies on macOS or
# Ubuntu/Debian. The C++ libraries themselves come via CMake FetchContent
# (SDL3, Dear ImGui) and git submodules (modules/: Capy, Corosio, automerge-cpp);
# this script only installs the system-level tools they need to build.
#
# zetta is C++26 and needs Clang 17+ (Apple's system Clang is too old — on macOS
# we install and build with Homebrew LLVM).
#
# Usage:  ./scripts/install-deps.sh
set -euo pipefail

note() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }

install_macos() {
    if ! command -v brew >/dev/null 2>&1; then
        warn "Homebrew not found — install it from https://brew.sh, then re-run."
        exit 1
    fi
    if ! xcode-select -p >/dev/null 2>&1; then
        note "Installing Xcode Command Line Tools (for the macOS SDK)…"
        xcode-select --install || true
    fi
    note "Installing build dependencies via Homebrew…"
    # llvm     — Clang 17+/C++26 (zetta auto-selects /opt/homebrew/opt/llvm)
    # openssl@3 — REQUIRED: inter-node mTLS (Corosio's TLS provider) + node
    #             identity minting (CTL-2, docs/cluster/security.md)
    # cmake/ninja/ccache — build system + compiler cache
    brew install cmake ninja llvm openssl@3 ccache libpq
    note "Done. zetta builds with Homebrew Clang automatically (no PATH change needed)."
}

install_ubuntu() {
    note "Installing build dependencies via apt…"
    sudo apt-get update
    sudo apt-get install -y \
        build-essential cmake ninja-build git ccache \
        clang lld \
        zlib1g-dev libssl-dev libpq-dev \
        libx11-dev libxext-dev libxrandr-dev libxi-dev libxcursor-dev \
        libxfixes-dev libxkbcommon-dev libwayland-dev wayland-protocols \
        libgl1-mesa-dev libegl1-mesa-dev libdbus-1-dev libudev-dev \
        libpipewire-0.3-dev libpulse-dev libasound2-dev

    # zetta needs Clang 17+ for C++26. Warn (don't fail) if the distro's is older.
    if command -v clang >/dev/null 2>&1; then
        local major
        major="$(clang --version | sed -n 's/.*clang version \([0-9]*\).*/\1/p' | head -1)"
        if [ -n "${major:-}" ] && [ "${major}" -lt 17 ]; then
            warn "clang ${major} is too old (need 17+). Install a newer one, e.g.:"
            warn "  wget https://apt.llvm.org/llvm.sh && chmod +x llvm.sh && sudo ./llvm.sh 19"
            warn "  then configure with -DCMAKE_CXX_COMPILER=clang++-19 -DCMAKE_C_COMPILER=clang-19"
        fi
    fi
    note "Done."
}

case "$(uname -s)" in
    Darwin) install_macos ;;
    Linux)
        if [ -r /etc/os-release ] && grep -qiE 'ubuntu|debian' /etc/os-release; then
            install_ubuntu
        else
            warn "Unsupported Linux distro. Install equivalents of: cmake, ninja,"
            warn "clang 17+, git, ccache, zlib, openssl, and SDL3's build deps."
            exit 1
        fi
        ;;
    *) warn "Unsupported OS: $(uname -s)"; exit 1 ;;
esac

note "Next: fetch submodules, then build —"
echo "    git submodule update --init --recursive"
echo "    cmake -S . -B build -G Ninja && cmake --build build"
