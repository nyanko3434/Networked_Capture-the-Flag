#!/usr/bin/env bash
set -euo pipefail

# ============================================================================
# Networked CTF — Build Setup Script
# Handles all dependencies and builds the project on a fresh Linux machine.
#
# Usage:
#   ./setup.sh              # Full setup: deps + build + tests
#   ./setup.sh --deps-only  # Install dependencies only
#   ./setup.sh --build-only # Build only (assume deps installed)
#   ./setup.sh --no-tests   # Skip running tests after build
#   ./setup.sh --server-only # Build server only (no X11/raylib needed)
# ============================================================================

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

DEPS_ONLY=false
BUILD_ONLY=false
NO_TESTS=false
SERVER_ONLY=false

for arg in "$@"; do
    case $arg in
        --deps-only)   DEPS_ONLY=true ;;
        --build-only)  BUILD_ONLY=true ;;
        --no-tests)    NO_TESTS=true ;;
        --server-only) SERVER_ONLY=true ;;
        --help|-h)
            echo "Usage: ./setup.sh [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --deps-only    Install system dependencies only"
            echo "  --build-only   Build only (assume deps installed)"
            echo "  --no-tests     Skip running tests after build"
            echo "  --server-only  Build server only (no X11/raylib needed)"
            echo "  --help         Show this help"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $arg${NC}"
            exit 1
            ;;
    esac
done

info()  { echo -e "${BLUE}[INFO]${NC}  $*"; }
ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
err()   { echo -e "${RED}[ERROR]${NC} $*"; }
step()  { echo -e "\n${GREEN}==>${NC} ${GREEN}$*${NC}"; }

# ============================================================================
# Detect distro
# ============================================================================

detect_distro() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        DISTRO="${ID:-unknown}"
        VERSION="${VERSION_ID:-}"
    elif command -v apt-get &>/dev/null; then
        DISTRO="debian"
    elif command -v dnf &>/dev/null; then
        DISTRO="fedora"
    elif command -v pacman &>/dev/null; then
        DISTRO="arch"
    else
        DISTRO="unknown"
    fi
    info "Detected distro: ${DISTRO} ${VERSION}"
}

# ============================================================================
# Install dependencies
# ============================================================================

install_deps_debian() {
    step "Installing dependencies (apt)"

    local packages=(
        build-essential
        cmake
        git
        libx11-dev
        libxrandr-dev
        libxinerama-dev
        libxcursor-dev
        libxi-dev
        libgl-dev
    )

    # Check which packages are already installed
    local to_install=()
    for pkg in "${packages[@]}"; do
        if ! dpkg -s "$pkg" &>/dev/null; then
            to_install+=("$pkg")
        fi
    done

    if [ ${#to_install[@]} -eq 0 ]; then
        ok "All dependencies already installed"
        return
    fi

    info "Installing: ${to_install[*]}"
    sudo apt-get update -qq
    sudo apt-get install -y -qq "${to_install[@]}"
    ok "Dependencies installed"
}

install_deps_fedora() {
    step "Installing dependencies (dnf)"

    local packages=(
        gcc-c++
        cmake
        git
        libX11-devel
        libXrandr-devel
        libXinerama-devel
        libXcursor-devel
        libXi-devel
        mesa-libGL-devel
    )

    local to_install=()
    for pkg in "${packages[@]}"; do
        if ! rpm -q "$pkg" &>/dev/null; then
            to_install+=("$pkg")
        fi
    done

    if [ ${#to_install[@]} -eq 0 ]; then
        ok "All dependencies already installed"
        return
    fi

    info "Installing: ${to_install[*]}"
    sudo dnf install -y -q "${to_install[@]}"
    ok "Dependencies installed"
}

install_deps_arch() {
    step "Installing dependencies (pacman)"

    local packages=(
        base-devel
        cmake
        git
        libx11
        libxrandr
        libxinerama
        libxcursor
        libxi
        mesa
    )

    local to_install=()
    for pkg in "${packages[@]}"; do
        if ! pacman -Qi "$pkg" &>/dev/null; then
            to_install+=("$pkg")
        fi
    done

    if [ ${#to_install[@]} -eq 0 ]; then
        ok "All dependencies already installed"
        return
    fi

    info "Installing: ${to_install[*]}"
    sudo pacman -S --noconfirm --needed "${to_install[@]}"
    ok "Dependencies installed"
}

install_deps() {
    case "$DISTRO" in
        ubuntu|debian|linuxmint|pop) install_deps_debian ;;
        fedora|rhel|centos|rocky|alma) install_deps_fedora ;;
        arch|manjaro|endeavouros) install_deps_arch ;;
        *)
            err "Unsupported distro: $DISTRO"
            err "Please install manually:"
            err "  - C++17 compiler (g++ >= 7 or clang++ >= 5)"
            err "  - CMake >= 3.16"
            err "  - Git"
            err "  - X11 dev libs: libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev"
            err "  - OpenGL dev: libgl-dev"
            exit 1
            ;;
    esac
}

# ============================================================================
# Verify toolchain
# ============================================================================

verify_toolchain() {
    step "Verifying toolchain"

    # CMake
    if ! command -v cmake &>/dev/null; then
        err "cmake not found. Install CMake >= 3.16"
        exit 1
    fi
    local cmake_ver
    cmake_ver=$(cmake --version | head -1 | grep -oP '\d+\.\d+')
    local cmake_major cmake_minor
    cmake_major=$(echo "$cmake_ver" | cut -d. -f1)
    cmake_minor=$(echo "$cmake_ver" | cut -d. -f2)
    if [ "$cmake_major" -lt 3 ] || { [ "$cmake_major" -eq 3 ] && [ "$cmake_minor" -lt 16 ]; }; then
        err "CMake >= 3.16 required, found $cmake_ver"
        exit 1
    fi
    ok "CMake $cmake_ver"

    # C++ compiler
    local compiler=""
    if command -v g++ &>/dev/null; then
        compiler="g++"
    elif command -v clang++ &>/dev/null; then
        compiler="clang++"
    else
        err "No C++ compiler found. Install g++ or clang++"
        exit 1
    fi
    local compiler_ver
    compiler_ver=$($compiler --version | head -1)
    ok "$compiler_ver"

    # Git
    if ! command -v git &>/dev/null; then
        err "git not found"
        exit 1
    fi
    ok "Git $(git --version | awk '{print $3}')"
}

# ============================================================================
# Build
# ============================================================================

build_project() {
    step "Building project"

    local build_dir="build"
    local cmake_args=()

    if [ "$SERVER_ONLY" = true ]; then
        cmake_args+=("-DCTF_BUILD_CLIENT=OFF")
        info "Server-only build (skipping raylib/X11)"
    fi

    # Create build directory
    mkdir -p "$build_dir"

    # Configure
    info "Running cmake..."
    cmake -S . -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Debug \
        "${cmake_args[@]}" \
        2>&1 | while IFS= read -r line; do
            echo "  $line"
        done

    # Build
    local nproc
    nproc=$(nproc 2>/dev/null || echo 4)
    info "Building with $nproc parallel jobs..."
    cmake --build "$build_dir" -j"$nproc" 2>&1 | while IFS= read -r line; do
        echo "  $line"
    done

    ok "Build complete"

    # List built binaries
    echo ""
    info "Built binaries:"
    [ -f "$build_dir/ctf_server" ] && ok "  $build_dir/ctf_server"
    if [ "$SERVER_ONLY" = false ]; then
        [ -f "$build_dir/ctf_client" ] && ok "  $build_dir/ctf_client"
        [ -f "$build_dir/ctf_bot" ]    && ok "  $build_dir/ctf_bot"
    fi
    [ -f "$build_dir/tests/ctf_tests" ] && ok "  $build_dir/tests/ctf_tests"
}

# ============================================================================
# Run tests
# ============================================================================

run_tests() {
    if [ "$NO_TESTS" = true ]; then
        info "Skipping tests (--no-tests)"
        return
    fi

    step "Running tests"

    local test_bin="build/tests/ctf_tests"
    if [ ! -f "$test_bin" ]; then
        warn "Test binary not found at $test_bin, skipping"
        return
    fi

    if "$test_bin"; then
        ok "All tests passed"
    else
        err "Tests failed!"
        exit 1
    fi
}

# ============================================================================
# Print usage instructions
# ============================================================================

print_usage() {
    echo ""
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN} Setup complete!${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
    echo "Run the game:"
    echo ""
    echo "  # Terminal 1 — start server"
    echo "  ./build/ctf_server --port 7777 --tick 30"
    echo ""
    echo "  # Terminal 2 — start client"
    echo "  ./build/ctf_client --host 127.0.0.1 --port 7777 --name player1"
    echo ""
    echo "  # Terminal 3 — fill with bots"
    echo "  ./build/ctf_bot --host 127.0.0.1 --port 7777 --count 9"
    echo ""
    echo "  # Host presses ENTER to start the match"
    echo ""
    echo "Run tests:"
    echo "  ./build/tests/ctf_tests"
    echo ""
    echo "Rebuild after changes:"
    echo "  cmake --build build -j"
    echo ""
}

# ============================================================================
# Main
# ============================================================================

main() {
    echo -e "${BLUE}"
    echo "  ╔═══════════════════════════════════════════╗"
    echo "  ║   Networked CTF — Build Setup             ║"
    echo "  ╚═══════════════════════════════════════════╝"
    echo -e "${NC}"

    detect_distro

    if [ "$BUILD_ONLY" = false ]; then
        install_deps
    else
        info "Skipping dependency installation (--build-only)"
    fi

    verify_toolchain
    build_project
    run_tests
    print_usage
}

main
