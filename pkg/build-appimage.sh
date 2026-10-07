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
#   ./pkg/build-appimage.sh [--clean] [--resume] [--verify-only] [--help]
#
# Options:
#   --clean        Remove the build directory before configuring
#   --resume       Skip the cmake --install step if the AppDir already exists
#                  and is fully populated. If some mods are missing, only the
#                  install step is run. If the AppDir is incomplete or
#                  corrupted, it is cleaned and the full install is run.
#   --verify-only  Check the current AppDir state and linuxdeploy binaries
#                  without building. Exit 0 if everything is verified,
#                  non-zero otherwise.
#   --build-type TYPE
#                  CMake build type: Debug, Release, RelWithDebInfo, MinSizeRel
#                  (default: Release)
#   --qt-version VERSION
#                  Qt version to use: 5, 6, or auto (default: auto)
#   --interactive
#                  Run in interactive mode, prompting for Qt version and build type
#   -y, --install-deps
#                  Automatically install missing dependencies using the
#                  system package manager (apt, dnf, yum, pacman, zypper,
#                  emerge, or apk). This may prompt for sudo/sudo-like
#                  permission. Without this flag, the script only prints
#                  installation hints and exits.
#   -h, --help     Show this help message
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
RESUME=0
VERIFY_ONLY=0
INSTALL_DEPS=0
AUTO_INSTALLED=0
BUILD_TYPE="Release"
QT_VERSION="auto"
INTERACTIVE=0

# State file for AppDir verification (stores checksums of installed files)
APPDIR_STATE_DIR="${PROJECT_DIR}/.kilo/appdir-state"
APPDIR_STATE_FILE="${APPDIR_STATE_DIR}/state.json"

# Resolved path to the qmake executable (set during dependency check, used at
# install time so linuxdeploy-plugin-qt can locate Qt).
qmake_path=""

# List of AppDir mods that linuxdeploy creates during the install step.
# Each entry is "name|check_path|type" where type is:
#   "dir"          - must exist as a directory
#   "file"         - must exist as a regular file
#   "elf"          - must exist as a regular ELF file (executable)
#   "nonempty_dir" - must exist and contain at least one file
#   "nonempty_file"- must exist and be non-empty
APPDIR_MODS=(
  "appdir_structure|${BUILD_DIR}/usr|dir"
  "apprun|${BUILD_DIR}/AppRun|file"
  "executables|${BUILD_DIR}/usr/bin|nonempty_dir"
  "drawpile_elf|${BUILD_DIR}/usr/bin/drawpile|elf"
  "drawpile_srv_elf|${BUILD_DIR}/usr/bin/drawpile-srv|elf"
  "shared_libraries|${BUILD_DIR}/usr/lib|nonempty_dir"
  "qt_plugins|${BUILD_DIR}/usr/plugins|nonempty_dir"
  "platform_plugins|${BUILD_DIR}/usr/plugins/platforms|nonempty_dir"
  "desktop_file|${BUILD_DIR}/usr/share/applications|nonempty_dir"
  "icons|${BUILD_DIR}/usr/share/icons|nonempty_dir"
  "translations|${BUILD_DIR}/usr/translations|nonempty_dir"
  "application_data|${BUILD_DIR}/usr/share/drawpile|nonempty_dir"
)

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

# ---------------------------------------------------------------------------
# Interactive prompts
# ---------------------------------------------------------------------------

# prompt_select: prompt user to select from a list of options
# Usage: prompt_select <variable_name> <prompt_text> <option1> <option2> ...
prompt_select() {
    local var_name="$1"
    local prompt_text="$2"
    shift 2
    local options=("$@")

    while true; do
        printf '%s\n' "$prompt_text"
        local i=1
        for opt in "${options[@]}"; do
            printf '  %d) %s\n' "$i" "$opt"
            i=$((i + 1))
        done
        printf 'Enter choice [1-%d]: ' "${#options[@]}"
        read -r choice
        if [[ "$choice" =~ ^[0-9]+$ ]] && [ "$choice" -ge 1 ] && [ "$choice" -le "${#options[@]}" ]; then
            eval "$var_name=\"${options[$((choice - 1))]}\""
            break
        else
            echo "Invalid choice. Please enter a number between 1 and ${#options[@]}."
        fi
    done
}

# run_interactive_config: prompt for Qt version and build type
run_interactive_config() {
    info "Interactive configuration mode"
    echo

    # Prompt for Qt version
    prompt_select QT_VERSION "Select Qt version:" "auto" "5" "6"

    # Prompt for build type
    prompt_select BUILD_TYPE "Select build type:" "Debug" "Release" "RelWithDebInfo" "MinSizeRel"

    echo
    info "Configuration selected:"
    printf '  Qt version: %s\n' "$QT_VERSION"
    printf '  Build type: %s\n' "$BUILD_TYPE"
    echo
}

# ---------------------------------------------------------------------------
# Package manager detection
# ---------------------------------------------------------------------------

# Detect the system package manager. Sets global variables:
#   PKG_MANAGER      - the command name (apt-get, dnf, pacman, etc.)
#   PKG_INSTALL_CMD  - the command prefix to install packages (e.g. "apt-get install -y")
#   PKG_UPDATE_CMD   - the command prefix to update package metadata (may be empty)
# Returns 0 on success, 1 if no supported package manager is found.
detect_package_manager() {
    PKG_MANAGER=""
    PKG_INSTALL_CMD=""
    PKG_UPDATE_CMD=""

    # apt-based: Debian, Ubuntu, etc.
    if command -v apt-get >/dev/null 2>&1; then
        PKG_MANAGER="apt-get"
        PKG_INSTALL_CMD="apt-get install -y"
        PKG_UPDATE_CMD="apt-get update"
        return 0
    fi

    # dnf: Fedora >= 22, RHEL >= 8, CentOS Stream
    if command -v dnf >/dev/null 2>&1; then
        PKG_MANAGER="dnf"
        PKG_INSTALL_CMD="dnf install -y"
        PKG_UPDATE_CMD="dnf check-update || true"
        return 0
    fi

    # yum: RHEL <= 7, CentOS <= 7, older Fedora
    if command -v yum >/dev/null 2>&1; then
        PKG_MANAGER="yum"
        PKG_INSTALL_CMD="yum install -y"
        PKG_UPDATE_CMD="yum makecache"
        return 0
    fi

    # pacman: Arch Linux, Manjaro, etc.
    if command -v pacman >/dev/null 2>&1; then
        PKG_MANAGER="pacman"
        PKG_INSTALL_CMD="pacman -S --noconfirm --needed"
        PKG_UPDATE_CMD="pacman -Sy"
        return 0
    fi

    # zypper: openSUSE
    if command -v zypper >/dev/null 2>&1; then
        PKG_MANAGER="zypper"
        PKG_INSTALL_CMD="zypper --non-interactive install"
        PKG_UPDATE_CMD="zypper --non-interactive refresh"
        return 0
    fi

    # emerge: Gentoo
    if command -v emerge >/dev/null 2>&1; then
        PKG_MANAGER="emerge"
        PKG_INSTALL_CMD="emerge --ask --autounmask"
        PKG_UPDATE_CMD="emerge --sync"
        return 0
    fi

    # apk: Alpine Linux
    if command -v apk >/dev/null 2>&1; then
        PKG_MANAGER="apk"
        PKG_INSTALL_CMD="apk add --no-cache"
        PKG_UPDATE_CMD=""
        return 0
    fi

    return 1
}

# Run a package manager command, optionally with elevated privileges.
# Usage: pm_run <command...>
pm_run() {
    local cmd_str="$*"

    # Determine if we need sudo
    local run_cmd="$cmd_str"
    if [ "$(id -u)" -ne 0 ]; then
        if command -v sudo >/dev/null 2>&1; then
            run_cmd="sudo $cmd_str"
        elif command -v doas >/dev/null 2>&1; then
            run_cmd="doas $cmd_str"
        else
            err "This script needs root privileges to install packages, but neither sudo nor doas is available."
            err "Re-run this script as root or install dependencies manually."
            exit 1
        fi
    fi

    info "Running: $run_cmd"
    eval "$run_cmd"
}

# ---------------------------------------------------------------------------
# Dependency-to-package mapping
# ---------------------------------------------------------------------------

# Package sets for each supported package manager.
# The keys are the generic dependency names (matching missing_apt_packages
# entries) and the values are the package names for that distribution.
get_package_set() {
    local pm="$1"
    local qt5_core_pkg qt6_core_pkg libzip_pkg kf5archive_pkg

    case "$pm" in
        apt-get)
            # Qt5 / Qt6 variant packages
            cat <<'EOFD'
cmake|cmake
gcc|gcc
g++|g++
git|git
pkg-config|pkg-config
objcopy|binutils
ninja-build|ninja-build
make|make
qtbase5-dev|qtbase5-dev
qtbase5-dev-tools|qtbase5-dev-tools
qttools5-dev-tools|qttools5-dev-tools
libqt5svg5-dev|libqt5svg5-dev
libqt5websockets5-dev|libqt5websockets5-dev
libqt5gui5|libqt5gui5
qt6-base-dev|qt6-base-dev
qt6-svg6-dev|qt6-svg6-dev
qt6-websockets6-dev|qt6-websockets6-dev
qt6-tools6-dev|qt6-tools6-dev
zlib1g-dev|zlib1g-dev
libzstd1-dev|libzstd1-dev
libwebp-dev|libwebp-dev
libzip-dev|libzip-dev
libkf5archive-dev|libkf5archive-dev
libsodium-dev|libsodium-dev
libswscale-dev|libswscale-dev
libavcodec-dev|libavcodec-dev
cargo|cargo
rustc|rustc
EOFD
            ;;
        dnf|yum)
            cat <<'EOFD'
cmake|cmake
gcc|gcc-c++
g++|gcc-c++
git|git
pkg-config|pkgconf-pkg-config
objcopy|binutils
ninja-build|ninja-build
make|make
qtbase5-dev|qt5-qtbase-devel
qtbase5-dev-tools|qt5-qtbase-dev
qttools5-dev-tools|qt5-qttools-devel
libqt5svg5-dev|qt5-qtsvg-devel
libqt5websockets5-dev|qt5-qtwebsockets-devel
libqt5gui5|qt5-qtbase-devel
qt6-base-dev|qt6-qtbase-devel
qt6-svg6-dev|qt6-qtsvg-devel
qt6-websockets6-dev|qt6-qtwebsockets-devel
qt6-tools6-dev|qt6-qttools-devel
zlib1g-dev|zlib-devel
libzstd1-dev|libzstd-devel
libwebp-dev|libwebp-devel
libzip-dev|libzip-devel
libkf5archive-dev|kf5-karchive-devel
libsodium-dev|libsodium-devel
libswscale-dev|ffmpeg-free-devel
libavcodec-dev|ffmpeg-free-devel
cargo|cargo
rustc|rustc
EOFD
            ;;
        pacman)
            cat <<'EOFD'
cmake|cmake
gcc|gcc
g++|gcc
git|git
pkg-config|pkgconf
objcopy|binutils
ninja-build|ninja
make|make
qtbase5-dev|qt5-base
qtbase5-dev-tools|qt5-base
qttools5-dev-tools|qt5-tools
libqt5svg5-dev|qt5-svg
libqt5websockets5-dev|qt5-websockets
libqt5gui5|qt5-base
qt6-base-dev|qt6-base
qt6-svg6-dev|qt6-svg
qt6-websockets6-dev|qt6-websockets
qt6-tools6-dev|qt6-tools
zlib1g-dev|zlib
libzstd1-dev|zstd
libwebp-dev|libwebp
libzip-dev|libzip
libkf5archive-dev|karchive-qt5
libsodium-dev|libsodium
libswscale-dev|ffmpeg
libavcodec-dev|ffmpeg
cargo|cargo
rustc|rustc
EOFD
            ;;
        zypper)
            cat <<'EOFD'
cmake|cmake
gcc|gcc
g++|gcc-c++
git|git
pkg-config|pkg-config
objcopy|binutils
ninja-build|ninja
make|make
qtbase5-dev|libqt5-qtbase-devel
qtbase5-dev-tools|libqt5-qtbase-devel
qttools5-dev-tools|libqt5-qttools-devel
libqt5svg5-dev|libqt5-qtsvg-devel
libqt5websockets5-dev|libqt5-qtwebsockets-devel
libqt5gui5|libqt5-qtbase-devel
qt6-base-dev|libqt6-qtbase-devel
qt6-svg6-dev|libqt6-qtsvg-devel
qt6-websockets6-dev|libqt6-qtwebsockets-devel
qt6-tools6-dev|libqt6-qttools-devel
zlib1g-dev|zlib-devel
libzstd1-dev|libzstd-devel
libwebp-dev|libwebp-devel
libzip-dev|libzip-devel
libkf5archive-dev|libKF5Archive-devel
libsodium-dev|libsodium-devel
libswscale-dev|ffmpeg-devel
libavcodec-dev|ffmpeg-devel
cargo|cargo
rustc|rustc
EOFD
            ;;
        emerge)
            cat <<'EOFD'
cmake|cmake
gcc|gcc
g++|gcc
git|git
pkg-config|pkg-config
objcopy|sys-apps/binutils
ninja-build|ninja
make|make
qtbase5-dev|dev-qt/qtbase
qtbase5-dev-tools|dev-qt/qtbase
qttools5-dev-tools|dev-qt/qttools
libqt5svg5-dev|dev-qt/qtsvg
libqt5websockets5-dev|dev-qt/qtwebsockets
libqt5gui5|dev-qt/qtbase
qt6-base-dev|dev-qt/qtbase
qt6-svg6-dev|dev-qt/qtsvg
qt6-websockets6-dev|dev-qt/qtwebsockets
qt6-tools6-dev|dev-qt/qttools
zlib1g-dev|sys-libs/zlib
libzstd1-dev|app-arch/zstd
libwebp-dev|media-libs/libwebp
libzip-dev|dev-libs/libzip
libkf5archive-dev|kde-frameworks/karchive
libsodium-dev|dev-libs/libsodium
libswscale-dev|media-video/ffmpeg
libavcodec-dev|media-video/ffmpeg
cargo|cargo
rustc|rust
EOFD
            ;;
        apk)
            cat <<'EOFD'
cmake|cmake
gcc|gcc
g++|g++
git|git
pkg-config|pkgconf
objcopy|binutils
ninja-build|ninja-build
make|make
qtbase5-dev|qt5-qtbase-dev
qtbase5-dev-tools|qt5-qtbase-dev
qttools5-dev-tools|qt5-qttools-dev
libqt5svg5-dev|qt5-qtsvg-dev
libqt5websockets5-dev|qt5-qtwebsockets-dev
libqt5gui5|qt5-qtbase-dev
qt6-base-dev|qt6-qtbase-dev
qt6-svg6-dev|qt6-qtsvg-dev
qt6-websockets6-dev|qt6-qtwebsockets-dev
qt6-tools6-dev|qt6-qttools-dev
zlib1g-dev|zlib-dev
libzstd1-dev|libzstd-dev
libwebp-dev|libwebp-dev
libzip-dev|libzip-dev
libsodium-dev|libsodium-dev
libswscale-dev|ffmpeg-dev
libavcodec-dev|ffmpeg-dev
cargo|cargo
rustc|rust
EOFD
            ;;
        *)
            echo ""
            ;;
    esac
}

# Look up the package name for a given apt package name and package manager.
# Usage: get_pkgname <apt_pkg> <pkg_manager>
get_pkgname() {
    local apt_pkg="$1" pm="$2"
    local mapping_file
    mapping_file="$(mktemp)"

    get_package_set "$pm" > "$mapping_file"

    # Look up by the apt package name (first field)
    local result
    result="$(awk -F'|' -v key="$apt_pkg" '$1==key { print $2; exit }' "$mapping_file")"
    rm -f "$mapping_file"
    echo "$result"
}

# Install missing dependencies using the detected package manager.
# Arguments are the apt-style package names from missing_apt_packages[].
install_dependencies() {
    local pm

    if ! detect_package_manager; then
        err "Could not detect a supported package manager (apt-get, dnf, yum, pacman, zypper, emerge, apk)."
        err "Please install the following dependencies manually:"
        for dep in "${missing_apt_packages[@]}"; do
            echo "  - $dep"
        done
        return 1
    fi

    info "Detected package manager: $PKG_MANAGER"

    # Translate apt package names to the target package manager's names
    local pkgs=()
    local translated=()
    local apt_pkg target_pkg
    for apt_pkg in "${missing_apt_packages[@]}"; do
        target_pkg="$(get_pkgname "$apt_pkg" "$PKG_MANAGER")"
        if [ -n "$target_pkg" ]; then
            if [ -z "${pkgs[*]:-}" ] || ! printf '%s\n' "${pkgs[@]}" | grep -qxF "$target_pkg"; then
                pkgs+=("$target_pkg")
            fi
        else
            warn "Could not map apt package '$apt_pkg' for $PKG_MANAGER; trying with original name"
            if [ -z "${pkgs[*]:-}" ] || ! printf '%s\n' "${pkgs[@]}" | grep -qxF "$apt_pkg"; then
                pkgs+=("$apt_pkg")
            fi
        fi
        translated+=("$apt_pkg -> ${target_pkg:-$apt_pkg}")
    done

    # Show translation map
    info "Package translation map:"
    for t in "${translated[@]}"; do
        printf '  %s\n' "$t"
    done

    # If no packages to install, just return success
    if [ ${#pkgs[@]} -eq 0 ]; then
        info "No packages to install (all missing dependencies are non-packaged tools like linuxdeploy)."
        return 0
    fi

    # Update package metadata
    if [ -n "$PKG_UPDATE_CMD" ]; then
        info "Updating package metadata..."
        pm_run "$PKG_UPDATE_CMD"
    fi

    # Install packages
    info "Installing packages: ${pkgs[*]}"
    pm_run "$PKG_INSTALL_CMD ${pkgs[*]}"

    info "Dependencies installed. Re-running dependency checks..."
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
        missing_apt_packages+=("ninja-build" "make")
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
        missing_apt_packages+=("qtbase5-dev" "qtbase5-dev-tools" "qttools5-dev-tools" "libqt5svg5-dev" "libqt5websockets5-dev")
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
            --resume)
                RESUME=1
                shift
                ;;
            --verify-only)
                VERIFY_ONLY=1
                shift
                ;;
            --build-type)
                if [ $# -lt 2 ]; then
                    err "Option --build-type requires an argument"
                    exit 2
                fi
                case "$2" in
                    Debug|Release|RelWithDebInfo|MinSizeRel)
                        BUILD_TYPE="$2"
                        ;;
                    *)
                        err "Invalid build type: $2"
                        err "Valid types: Debug, Release, RelWithDebInfo, MinSizeRel"
                        exit 2
                        ;;
                esac
                shift 2
                ;;
            --qt-version)
                if [ $# -lt 2 ]; then
                    err "Option --qt-version requires an argument"
                    exit 2
                fi
                case "$2" in
                    5|6|auto)
                        QT_VERSION="$2"
                        ;;
                    *)
                        err "Invalid Qt version: $2"
                        err "Valid values: 5, 6, auto"
                        exit 2
                        ;;
                esac
                shift 2
                ;;
            --interactive)
                INTERACTIVE=1
                shift
                ;;
            -y|--install-deps)
                INSTALL_DEPS=1
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

    if [ "$INTERACTIVE" -eq 1 ]; then
        run_interactive_config
    fi

    info "Drawpile AppImage build script"
    log "Project directory: ${PROJECT_DIR}"
    log "Build directory:   ${BUILD_DIR}"
    log

    check_dependencies

    if [ ${#required_missing[@]} -gt 0 ]; then
        if [ "$INSTALL_DEPS" -eq 1 ]; then
            if [ ${#missing_apt_packages[@]} -gt 0 ]; then
                install_dependencies || {
                    err "Automatic dependency installation failed."
                    err "Install the missing dependencies manually and re-run this script."
                    exit 1
                }
                # Reset tracking arrays and re-check after installation
                required_missing=()
                optional_missing=()
                missing_apt_packages=()
                AUTO_INSTALLED=1
                check_dependencies
            else
                warn "Some required dependencies are missing but cannot be"
                warn "installed automatically (no package mapping available)."
            fi
        fi

        if [ ${#required_missing[@]} -gt 0 ]; then
            print_missing_summary
            err "Some required dependencies are missing."
            if [ "$INSTALL_DEPS" -eq 1 ]; then
                err "Automatic installation did not resolve all dependencies."
                err "Install the remaining dependencies manually and re-run."
            else
                err "Run this script with --install-deps (-y) to install them automatically,"
                err "or install them manually (possibly with sudo) and re-run this script."
            fi
            exit 1
        fi
    fi

    if [ "$AUTO_INSTALLED" -eq 1 ]; then
        info "Dependencies were installed automatically."
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
        if ! rm -rf "${BUILD_DIR}" 2>/dev/null; then
            warn "Could not remove build directory (permission denied)."
            warn "Some files may be owned by root from a previous sudo run."
            warn "Try running: sudo rm -rf ${BUILD_DIR}"
            exit 1
        fi
    fi

    # ------------------------------------------------------------------
    # Step 1: Configure
    # ------------------------------------------------------------------
    info "Step 1/3: Configuring CMake (${BUILD_TYPE}, APPIMAGE=ON)..."

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

    # Qt version selection
    local qt_args=()
    case "$QT_VERSION" in
        5)
            qt_args+=(-DDP_MIN_QT_VERSION=5.12)
            ;;
        6)
            qt_args+=(-DDP_MIN_QT_VERSION=6.0)
            ;;
        auto)
            # Auto-detect (default CMake behavior)
            ;;
    esac

    cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
        "${generator_args[@]}" \
        "${tools_args[@]}" \
        "${prefix_args[@]}" \
        "${qt_args[@]}" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
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
    cmake --build "${BUILD_DIR}" --parallel "$(nproc_count)" --config "${BUILD_TYPE}"

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

    env "${install_env[@]}" cmake --install "${BUILD_DIR}" --config "${BUILD_TYPE}"

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
