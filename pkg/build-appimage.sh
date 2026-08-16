#!/usr/bin/env bash
#
# build-appimage.sh - Automate the creation of a Drawpile AppImage.
#
# This script first checks for all the necessary build and packaging
# dependencies. If any required dependency is missing, it prints the list of
# missing items together with an installation hint (a sudo apt-get command)
# and exits. It does NOT install anything itself.
#
# Once every required dependency is present, the script configures, builds,
# and installs the project. The CMake install step invokes linuxdeploy,
# which produces a self-contained AppImage named
#   Drawpile-<version>-x86_64.AppImage
# in the project root directory.
#
# Usage:
#   ./pkg/build-appimage.sh [--clean] [--help]
#
# Options:
#   --clean     Remove the build directory before configuring
#   -h, --help  Show this help message
#

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
LINUXDEPLOY_DIR="${PROJECT_DIR}/.linuxdeploy-bin"
LINUXDEPLOY_APP="linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_QT_PLUGIN_APP="linuxdeploy-plugin-qt-x86_64.AppImage"
LINUXDEPLOY_RELEASE_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous"
LINUXDEPLOY_QT_RELEASE_URL="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous"

CLEAN=0

# Resolved path to the qmake executable (set during dependency check, used at
# install time so linuxdeploy-plugin-qt can locate Qt).
qmake_path=""

# ---------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------
log()   { printf '%s\n' "$*"; }
info()  { printf '\033[1;34m[info]\033[0m %s\n' "$*"; }
ok()    { printf '  \033[1;32m[OK]\033[0m %s\n' "$*"; }
warn()  { printf '  \033[1;33m[WARN]\033[0m %s\n' "$*"; }
miss()  { printf '  \033[1;31m[MISS]\033[0m %s\n' "$*"; }

if ! { [ -t 1 ] && command -v tput >/dev/null 2>&1 && tput colors >/dev/null 2>&1; }; then
    info()  { printf '[info] %s\n' "$*"; }
    ok()    { printf '  [OK] %s\n' "$*"; }
    warn()  { printf '  [WARN] %s\n' "$*"; }
    miss()  { printf '  [MISS] %s\n' "$*"; }
fi

err() { printf 'Error: %s\n' "$*" >&2; }

# ---------------------------------------------------------------------------
# Dependency tracking
# ---------------------------------------------------------------------------
required_missing=()     # human-readable names of missing required deps
optional_missing=()     # human-readable names of missing optional deps
missing_apt_packages=()  # apt package names corresponding to missing deps

# check_command NAME [APT_PKG]
check_command() {
    local cmd="$1" pkg="${2:-}"
    if command -v "$cmd" >/dev/null 2>&1; then
        ok "$cmd -> $(command -v "$cmd")"
    else
        miss "$cmd"
        required_missing+=("$cmd")
        [ -n "$pkg" ] && missing_apt_packages+=("$pkg")
    fi
}

# check_command_optional NAME
check_command_optional() {
    local cmd="$1"
    if command -v "$cmd" >/dev/null 2>&1; then
        ok "$cmd -> $(command -v "$cmd")"
    else
        warn "$cmd (optional - enables additional features)"
        optional_missing+=("$cmd")
    fi
}

# check_pkgconfig MODULE [APT_PKG]
check_pkgconfig() {
    local module="$1" pkg="${2:-}"
    if pkg-config --exists "$module" 2>/dev/null; then
        ok "$module -> $(pkg-config --modversion "$module" 2>/dev/null)"
    else
        miss "$module"
        required_missing+=("$module")
        [ -n "$pkg" ] && missing_apt_packages+=("$pkg")
    fi
}

# check_pkgconfig_optional MODULE
check_pkgconfig_optional() {
    local module="$1"
    if pkg-config --exists "$module" 2>/dev/null; then
        ok "$module -> $(pkg-config --modversion "$module" 2>/dev/null)"
    else
        warn "$module (optional - enables extra features)"
        optional_missing+=("$module")
    fi
}

# find_command: search for the first available command name from a list.
# Prints the resolved path to stdout (empty if none found).
find_command() {
    local c
    for c in "$@"; do
        if command -v "$c" >/dev/null 2>&1; then
            command -v "$c"
            return 0
        fi
    done
    return 1
}

# ---------------------------------------------------------------------------
# Dependency checks
# ---------------------------------------------------------------------------
check_dependencies() {
    info "Checking build tools..."
    check_command cmake      cmake
    check_command gcc        gcc
    check_command g++        g++
    check_command git        git
    check_command pkg-config pkg-config
    check_command objcopy    binutils

    info "Checking build system generator (need ninja or make)..."
    if command -v ninja >/dev/null 2>&1; then
        ok "ninja -> $(command -v ninja)"
    elif command -v make >/dev/null 2>&1; then
        ok "make -> $(command -v make) (fallback generator; ninja recommended)"
    else
        miss "ninja or make (at least one required)"
        required_missing+=("ninja or make")
        missing_apt_packages+=("ninja-build make")
    fi

    info "Checking CMake version (requires >= 3.18)..."
    if command -v cmake >/dev/null 2>&1; then
        local cmake_version
        cmake_version="$(cmake --version 2>/dev/null | head -1 | awk '{print $3}')"
        ok "cmake $cmake_version"
        if ! printf '3.18\n%s\n' "$cmake_version" | sort -V -C; then
            miss "cmake version >= 3.18 required (found $cmake_version)"
            required_missing+=("cmake>=3.18")
        fi
    fi

    info "Checking Qt development libraries..."
    local qt_detected=0
    if pkg-config --exists Qt5Core 2>/dev/null; then
        ok "Qt5 detected -> $(pkg-config --modversion Qt5Core 2>/dev/null)"
        check_pkgconfig Qt5Svg          libqt5svg5-dev
        check_pkgconfig Qt5WebSockets   libqt5websockets5-dev
        check_pkgconfig Qt5Gui         libqt5gui5
        qt_detected=1
    elif pkg-config --exists Qt6Core 2>/dev/null; then
        ok "Qt6 detected -> $(pkg-config --modversion Qt6Core 2>/dev/null)"
        check_pkgconfig Qt6Svg          qt6-svg6-dev
        check_pkgconfig Qt6WebSockets   qt6-websockets6-dev
        check_pkgconfig Qt6Gui         qt6-base-dev
        qt_detected=1
    else
        miss "Qt5 or Qt6 development libraries (Qt5Core / Qt6Core not found)"
        required_missing+=("Qt5 or Qt6 development libraries")
        missing_apt_packages+=("qtbase5-dev qtbase5-dev-tools qttools5-dev-tools libqt5svg5-dev libqt5websockets5-dev")
    fi

    if [ "$qt_detected" -eq 1 ]; then
        qmake_path="$(find_command qmake qmake6 qmake-qt6 qmake-qt5 || true)"
        if [ -n "$qmake_path" ]; then
            ok "qmake -> $qmake_path"
        else
            warn "qmake not found (linuxdeploy-plugin-qt needs it)"
            required_missing+=("qmake")
        fi
    fi

    info "Checking other required libraries..."
    check_pkgconfig zlib          zlib1g-dev
    check_pkgconfig libzstd       libzstd1-dev
    check_pkgconfig libwebp       libwebp-dev

    # ZIP backend depends on Qt version (see cmake/DrawdanceOptions.cmake):
    # Qt5 → KF5Archive, Qt6 → libzip
    if pkg-config --exists Qt6Core 2>/dev/null; then
        check_pkgconfig libzip libzip-dev
    elif pkg-config --exists Qt5Core 2>/dev/null; then
        check_pkgconfig Qt5Archive libkf5archive-dev
    fi

    info "Checking optional libraries (recommended for full functionality)..."
    check_pkgconfig_optional libsodium
    check_pkgconfig_optional libswscale
    check_pkgconfig_optional libavcodec
    check_command_optional cargo
    check_command_optional rustc

    info "Checking AppImage packaging tools..."
    local ld_found=0 ld_qt_found=0
    if [ -x "${LINUXDEPLOY_DIR}/${LINUXDEPLOY_APP}" ]; then
        ok "linuxdeploy -> ${LINUXDEPLOY_DIR}/${LINUXDEPLOY_APP}"
        ld_found=1
    elif command -v linuxdeploy-x86_64.AppImage >/dev/null 2>&1; then
        ok "linuxdeploy -> $(command -v linuxdeploy-x86_64.AppImage)"
        ld_found=1
    else
        miss "linuxdeploy-x86_64.AppImage"
        required_missing+=("linuxdeploy-x86_64.AppImage")
    fi
    if [ -x "${LINUXDEPLOY_DIR}/${LINUXDEPLOY_QT_PLUGIN_APP}" ]; then
        ok "linuxdeploy-plugin-qt -> ${LINUXDEPLOY_DIR}/${LINUXDEPLOY_QT_PLUGIN_APP}"
        ld_qt_found=1
    elif command -v linuxdeploy-plugin-qt-x86_64.AppImage >/dev/null 2>&1; then
        ok "linuxdeploy-plugin-qt -> $(command -v linuxdeploy-plugin-qt-x86_64.AppImage)"
        ld_qt_found=1
    else
        miss "linuxdeploy-plugin-qt-x86_64.AppImage"
        required_missing+=("linuxdeploy-plugin-qt-x86_64.AppImage")
    fi
    if [ $ld_found -eq 0 ] || [ $ld_qt_found -eq 0 ]; then
        log
        log "  Download linuxdeploy release:"
        log "    ${LINUXDEPLOY_RELEASE_URL}/${LINUXDEPLOY_APP}"
        log "    ${LINUXDEPLOY_QT_RELEASE_URL}/${LINUXDEPLOY_QT_PLUGIN_APP}"
        log "  into ${LINUXDEPLOY_DIR}/ and make them executable (chmod +x)."
    fi
}

# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
apt_available() { command -v apt-get >/dev/null 2>&1; }

print_missing_summary() {
    echo
    echo "=========================================================="
    echo "  MISSING DEPENDENCIES"
    echo "=========================================================="
    if [ ${#required_missing[@]} -gt 0 ]; then
        echo
        echo "Required dependencies missing:"
        for dep in "${required_missing[@]}"; do
            echo "  - $dep"
        done
    fi
    if [ ${#optional_missing[@]} -gt 0 ]; then
        echo
        echo "Optional (recommended) dependencies missing:"
        for dep in "${optional_missing[@]}"; do
            echo "  - $dep"
        done
    fi
    echo
    echo "=========================================================="
    echo "  INSTALLATION HINT"
    echo "=========================================================="
    if apt_available && [ ${#missing_apt_packages[@]} -gt 0 ]; then
        local -A seen=()
        local pkgs=()
        local p
        for p in "${missing_apt_packages[@]}"; do
            [ -z "${seen[$p]:-}" ] && pkgs+=("$p") && seen["$p"]=1
        done
        local joined
        joined="$(printf '%s ' "${pkgs[@]}")"
        echo "On Debian/Ubuntu, install them with (you may need sudo):"
        echo "  sudo apt-get update && sudo apt-get install --no-install-recommends $joined"
    else
        echo "Please install the missing dependencies using your distribution's"
        echo "package manager (you may need to run with sudo) and re-run this script."
    fi
    echo "=========================================================="
}

# ---------------------------------------------------------------------------
# Build helpers
# ---------------------------------------------------------------------------
get_version() {
    local version
    version="$(git -C "${PROJECT_DIR}" describe --always --tags --dirty 2>/dev/null || true)"
    if [ -z "$version" ]; then
        version="$(git -C "${PROJECT_DIR}" rev-parse --short HEAD 2>/dev/null || echo 'unknown')"
        warn "No version tags found; using git short hash: $version"
    fi
    echo "$version"
}

nproc_count() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    else
        echo 2
    fi
}

find_qt_plugins_dir() {
    local dir
    dir="$(pkg-config --variable=plugindir Qt6Core 2>/dev/null || true)"
    [ -z "$dir" ] && dir="$(pkg-config --variable=plugindir Qt5Core 2>/dev/null || true)"
    if [ -n "$dir" ] && [ -d "$dir" ]; then
        echo "$dir"
        return 0
    fi
    # Fallback: query qmake for QT_INSTALL_PLUGINS
    local qbin=""
    for c in "${qmake_path:-}" qmake6 qmake-qt6 qmake-qt5 qmake; do
        if [ -n "$c" ] && command -v "$c" >/dev/null 2>&1; then
            qbin="$c"
            break
        fi
    done
    if [ -n "$qbin" ]; then
        dir="$("$qbin" -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
        if [ -n "$dir" ] && [ -d "$dir" ]; then
            echo "$dir"
            return 0
        fi
    fi
}

create_system_lib_stubs() {
    local qt_plugins_dir
    qt_plugins_dir="$(find_qt_plugins_dir)"
    if [ -z "$qt_plugins_dir" ] || [ ! -d "$qt_plugins_dir" ]; then
        return 0
    fi
    info "Scanning Qt plugins for missing system library dependencies..."

    local stub_dir="${BUILD_DIR}/.lib-stubs/lib"
    mkdir -p "$stub_dir"

    local missing
    missing="$(find "$qt_plugins_dir" -name '*.so' -type f -exec ldd {} \; 2>/dev/null \
        | grep 'not found' \
        | awk '{print $1}' \
        | sort -u || true)"

    if [ -z "$missing" ]; then
        ok "No missing system library dependencies detected."
        rm -rf "${stub_dir%/lib}"
        return 0
    fi

    warn "Found missing system libraries used by Qt plugins:"
    local lib
    for lib in $missing; do
        warn "  $lib (creating stub to satisfy linuxdeploy deployment)"
        echo "void __stub_${lib//[-.]/_}() {}" > "${stub_dir}/src_${lib}.c"
        gcc -shared -fPIC -Wl,-soname,"$lib" -o "${stub_dir}/${lib}" "${stub_dir}/src_${lib}.c" 2>/dev/null || true
    done

    info "Stub libraries placed in: ${stub_dir}"
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --clean)
                CLEAN=1
                shift
                ;;
            -h|--help)
                sed -n '3,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
                exit 0
                ;;
            *)
                err "Unknown option: $1"
                err "Run '$0 --help' for usage."
                exit 2
                ;;
        esac
    done
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
    parse_args "$@"

    info "Drawpile AppImage build script"
    log "Project directory: ${PROJECT_DIR}"
    log "Build directory:   ${BUILD_DIR}"
    log

    check_dependencies

    if [ ${#required_missing[@]} -gt 0 ]; then
        print_missing_summary
        err "Some required dependencies are missing."
        err "Install them (possibly with sudo) and re-run this script."
        exit 1
    fi

    info "All required dependencies are satisfied."

    # Make linuxdeploy discoverable by CMake's find_program()
    if [ -d "${LINUXDEPLOY_DIR}" ]; then
        export PATH="${LINUXDEPLOY_DIR}:${PATH}"
    fi

    local version
    version="$(get_version)"
    local appimage_name="Drawpile-${version}-x86_64.AppImage"
    log "Project version: ${version}"

    if [ "$CLEAN" -eq 1 ]; then
        info "Cleaning build directory: ${BUILD_DIR}"
        rm -rf "${BUILD_DIR}"
    fi

    # ------------------------------------------------------------------
    # Step 1: Configure
    # ------------------------------------------------------------------
    info "Step 1/3: Configuring CMake (Release, APPIMAGE=ON)..."

    local generator_args=()
    if command -v ninja >/dev/null 2>&1; then
        generator_args=(-G Ninja)
    else
        generator_args=(-G "Unix Makefiles")
    fi

    # Enable command-line tools only when the Rust toolchain is available,
    # since CMake requires cargo to build them (cmake_dependent_option).
    local tools_args=()
    if command -v cargo >/dev/null 2>&1; then
        tools_args=(-DTOOLS=ON)
    fi

    # Allow the user to supply a custom CMAKE_PREFIX_PATH (e.g. when Qt lives
    # in a non-standard prefix). DrawpileDistBuild.cmake automatically uses it
    # to construct LD_LIBRARY_PATH for the linuxdeploy step.
    local prefix_args=()
    if [ -n "${CMAKE_PREFIX_PATH:-}" ]; then
        prefix_args=(-DCMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH}")
    fi

    cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
        "${generator_args[@]}" \
        "${tools_args[@]}" \
        "${prefix_args[@]}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCLIENT=ON \
        -DSERVER=ON \
        -DSERVERGUI=ON \
        -DAPPIMAGE=ON \
        -DUSE_GENERATORS=OFF \
        -DSOURCE_ASSETS=OFF \
        -DBUILD_VERSION="${version}" \
        -DCMAKE_INSTALL_PREFIX="${BUILD_DIR}"

    # ------------------------------------------------------------------
    # Step 2: Build
    # ------------------------------------------------------------------
    info "Step 2/3: Building the project (this may take a while)..."
    cmake --build "${BUILD_DIR}" --parallel "$(nproc_count)"

    # ------------------------------------------------------------------
    # Step 3: Install + package (linuxdeploy produces the AppImage)
    # ------------------------------------------------------------------
    info "Step 3/3: Installing and packaging the AppImage..."
    if [ -n "$qmake_path" ]; then
        export QMAKE="$qmake_path"
    fi

    # On some distributions (e.g. Arch Linux derivatives) the Qt6 image-format
    # plugins pull in optional system libraries (jxrlib, libheif, etc.) that
    # are not installed as hard dependencies. linuxdeploy fails when it cannot
    # find these. We create empty stub shared libraries to satisfy the
    # ELF dependency walker so the deployment can proceed. On Debian/Ubuntu
    # this is a no-op because all required libraries are already present.
    create_system_lib_stubs

    # Patch cmake_install.cmake so linuxdeploy picks up the stub libraries
    # via LD_LIBRARY_PATH during the deployment step.
    local stub_lib_dir="${BUILD_DIR}/.lib-stubs/lib"
    if [ -d "${stub_lib_dir}" ]; then
        local cmake_install="${BUILD_DIR}/cmake_install.cmake"
        if [ -f "$cmake_install" ]; then
            sed -i \
                "s|set(extra_env \"LD_LIBRARY_PATH=\([^\"]*\)\")|set(extra_env \"LD_LIBRARY_PATH=${stub_lib_dir}:\1\")|" \
                "$cmake_install"
        fi
    fi

    # Match the CI configuration: deploy the Qt SVG plugin and pass the
    # version so linuxdeploy names the AppImage correctly.
    local install_env=()
    install_env+=("EXTRA_QT_PLUGINS=svg;")
    install_env+=("VERSION=${version}")
    # Some distributions ship ELF binaries with relocation features that the
    # linuxdeploy bundled strip cannot handle (e.g. .relr.dyn on newer
    # toolchains). Retrying without stripping produces a still-working
    # (slightly larger) AppImage instead of failing the build.
    install_env+=("NO_STRIP=1")

    env "${install_env[@]}" cmake --install "${BUILD_DIR}" --config Release

    # ------------------------------------------------------------------
    # Locate and report the resulting AppImage
    # ------------------------------------------------------------------
    local appimage_path=""
    local found
    found="$(find "${BUILD_DIR}" "${PROJECT_DIR}" \
        -maxdepth 2 -name "Drawpile-*.AppImage" -type f 2>/dev/null | head -1 || true)"
    if [ -n "$found" ]; then
        appimage_path="$found"
    fi

    if [ -n "$appimage_path" ]; then
        local dest="${PROJECT_DIR}/${appimage_name}"
        if [ "$appimage_path" != "$dest" ] && [ -f "$appimage_path" ]; then
            mv -f "$appimage_path" "$dest"
        fi
        info "AppImage created successfully:"
        ok "${dest}"
        ok "$(du -h "$dest" | cut -f1)"
        log
        log "You can now run it with:"
        log "  ${dest}"
    else
        warn "The AppImage was not found in the expected location."
        warn "Check the build output above for errors."
        warn "The AppDir layout is at: ${BUILD_DIR}/"
        exit 1
    fi
}

main "$@"
