#!/usr/bin/env bash
set -euo pipefail

APPIMAGE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "${APPIMAGE_DIR}")"
BUILD_DIR="${PROJECT_DIR}/build"
STAGE_DIR="$(cd "${PROJECT_DIR}" && pwd)/appimage-stage"
LINUXDEPLOY_DIR="${PROJECT_DIR}/.linuxdeploy-bin"
CLEAN_CACHE=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [--clean]

  --clean    Remove the downloaded linuxdeploy cache
             (~.linuxdeploy-bin/) after creating the AppImage.
EOF
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean)
            CLEAN_CACHE=1
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            ;;
    esac
done

VERSION="$(git describe --always --tags --dirty 2>/dev/null || echo 'dev')"

if [[ ! -d "${BUILD_DIR}" ]]; then
    echo "Error: build directory not found at ${BUILD_DIR}. Run cmake --build first." >&2
    exit 1
fi

if ! command -v appimagetool >/dev/null 2>&1; then
    echo "Error: appimagetool is not installed. Install it to continue." >&2
    exit 1
fi

if ! command -v objcopy >/dev/null 2>&1; then
    echo "Error: objcopy (binutils) is not installed. Install it to continue." >&2
    exit 1
fi

mkdir -p "${LINUXDEPLOY_DIR}"
export PATH="${LINUXDEPLOY_DIR}:${PATH}"

rm -rf "${STAGE_DIR}"

echo "Reconfiguring CMake for AppImage packaging..."
cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
    -DAPPIMAGE=ON \
    -DCMAKE_INSTALL_PREFIX="${STAGE_DIR}" \
    -DCMAKE_BUILD_TYPE=Release

echo "Populating AppDir and deploying Qt dependencies via linuxdeploy..."
set +e
EXTRA_QT_PLUGINS="svg;" \
VERSION="${VERSION}" \
cmake --install "${BUILD_DIR}" --config Release
LINUXDEPLOY_EXIT=$?
set -e

if [[ ! -d "${STAGE_DIR}" ]]; then
    echo "Error: staging directory was not created." >&2
    exit 1
fi

echo "Fixing bundled libraries with .relr.dyn sections..."
find "${STAGE_DIR}" -type f -name '*.so*' -exec objcopy --remove-section=.relr.dyn {} \; || true

if [[ ! -f "${BUILD_DIR}/pkg/custom-apprun.sh" ]]; then
    echo "Error: custom AppRun script not found at ${BUILD_DIR}/pkg/custom-apprun.sh" >&2
    exit 1
fi
cp "${BUILD_DIR}/pkg/custom-apprun.sh" "${STAGE_DIR}/AppRun"
chmod +x "${STAGE_DIR}/AppRun"

if [[ -f "${STAGE_DIR}/usr/share/applications/net.drawpile.drawpile.desktop" ]]; then
    cp "${STAGE_DIR}/usr/share/applications/net.drawpile.drawpile.desktop" "${STAGE_DIR}/net.drawpile.drawpile.desktop"
fi

if [[ -f "${PROJECT_DIR}/src/desktop/icons/drawpile.png" ]]; then
    cp "${PROJECT_DIR}/src/desktop/icons/drawpile.png" "${STAGE_DIR}/drawpile.png"
fi

APPIMAGE_NAME="Drawpile-${VERSION}-x86_64.AppImage"
echo "Creating AppImage: ${APPIMAGE_NAME}"
appimagetool "${STAGE_DIR}" "${PROJECT_DIR}/${APPIMAGE_NAME}"

rm -rf "${STAGE_DIR}"

if [[ "${CLEAN_CACHE}" -eq 1 ]]; then
    echo "Cleaning linuxdeploy cache to liberate space..."
    rm -rf "${LINUXDEPLOY_DIR}"
    echo "Cache removed."
fi

echo "Success! AppImage created at: ${PROJECT_DIR}/${APPIMAGE_NAME}"
