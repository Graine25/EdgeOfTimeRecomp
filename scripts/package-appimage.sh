#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

PRESET="${1:-linux-amd64-relwithdebinfo}"
APP_NAME="EdgeOfTimeRecomp"
BUILD_DIR="out/build/${PRESET}"
APPIMAGE_ARCH="${APPIMAGE_ARCH:-x86_64}"
DOWNLOADS="${XDG_DOWNLOAD_DIR:-$HOME/Downloads}"

if [ ! -x "${BUILD_DIR}/${APP_NAME}" ]; then
    echo "[FAILED] No build at ${BUILD_DIR}. Build first (scripts/build.sh), then run this." >&2
    exit 1
fi
if [ ! -f "${BUILD_DIR}/program_files.txt" ]; then
    echo "[FAILED] ${BUILD_DIR}/program_files.txt is missing; cannot tell what to package." >&2
    exit 1
fi

version="$(grep -m1 'REEOT_VERSION_STRING' "${BUILD_DIR}/generated/core/build_info.h" | sed -n 's/.*"\([^"]*\)".*/\1/p')"
stamp="$(sed -n '1p' "${BUILD_DIR}/build_stamp.txt" 2>/dev/null | tr -d '\r')"
OUT_FILE="${APP_NAME}-v${version:-unknown}${stamp:+-$stamp}-${APPIMAGE_ARCH}.AppImage"

host_glibc="$(ldd --version 2>/dev/null | sed -n '1p' | grep -oE '[0-9]+\.[0-9]+$' || true)"
host_glibcxx="$(strings -n 8 "$(ldd "${BUILD_DIR}/${APP_NAME}" | awk '/libstdc\+\+/ {print $3}')" 2>/dev/null \
    | grep -oE '^GLIBCXX_3\.[0-9.]+$' | sort -V | tail -1 | sed 's/GLIBCXX_//')"
MAX_GLIBC="${MAX_GLIBC:-${host_glibc:-2.38}}"
MAX_GLIBCXX="${MAX_GLIBCXX:-${host_glibcxx:-3.4.32}}"

STAGING="$(mktemp -d)"
APPDIR="${STAGING}/AppDir"
trap 'rm -rf "${STAGING}"' EXIT

mkdir -p "${APPDIR}/usr/bin" "${APPDIR}/usr/lib"
while IFS= read -r f; do
    f="${f%$'\r'}"
    [ -n "$f" ] || continue
    if [ ! -e "${BUILD_DIR}/${f}" ]; then
        echo "[FAILED] ${f} is listed in program_files.txt but not in ${BUILD_DIR}." >&2
        exit 1
    fi
    cp -a "${BUILD_DIR}/${f}" "${APPDIR}/usr/bin/"
done < "${BUILD_DIR}/program_files.txt"
cp "${BUILD_DIR}/program_files.txt" "${APPDIR}/usr/bin/"

for f in "${APPDIR}/usr/bin/${APP_NAME}" "${APPDIR}/usr/bin"/*.so; do
    if ldd "${f}" | grep -q "not found"; then
        ldd "${f}" | grep "not found" >&2
        echo "[FAILED] unresolved runtime dependencies of $(basename "${f}")" >&2
        exit 1
    fi
done

version_gt() {
    [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$1" ]
}

violations=()
for f in "${APPDIR}/usr/bin"/*; do
    [ -f "${f}" ] || continue
    while IFS= read -r sym; do
        case "${sym}" in
            GLIBCXX_*) version_gt "${sym#GLIBCXX_}" "${MAX_GLIBCXX}" && violations+=("$(basename "${f}") requires ${sym} (ceiling GLIBCXX_${MAX_GLIBCXX})") ;;
            GLIBC_*)   version_gt "${sym#GLIBC_}"   "${MAX_GLIBC}"   && violations+=("$(basename "${f}") requires ${sym} (ceiling GLIBC_${MAX_GLIBC})") ;;
        esac
    done < <(objdump -T "${f}" 2>/dev/null | grep -oE 'GLIBC(XX)?_[0-9]+[.][0-9]+([.][0-9]+)?' | sort -u)
done
if [ ${#violations[@]} -gt 0 ]; then
    printf '[FAILED] %s\n' "${violations[@]}" >&2
    echo "refusing to package: these would fail to start on the target systems." >&2
    echo "Raise the ceiling (MAX_GLIBC / MAX_GLIBCXX) only for a package meant for hosts this new." >&2
    exit 1
fi

for f in "${APPDIR}/usr/bin"/*; do
    [ -f "${f}" ] || continue
    ldd "${f}" 2>/dev/null | grep -E 'libstdc[+][+][.]so[.]6|libgcc_s[.]so[.]1' || true
done | awk '{print $3}' | sort -u | while IFS= read -r lib; do
    [ -f "${lib}" ] || continue
    cp "$(readlink -f "${lib}")" "${APPDIR}/usr/lib/$(basename "${lib}")"
    echo "vendoring $(basename "${lib}")"
done

cat > "${APPDIR}/AppRun" <<'APPRUN'
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/usr/bin/EdgeOfTimeRecomp" "$@"
APPRUN
chmod +x "${APPDIR}/AppRun"

cat > "${APPDIR}/${APP_NAME}.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=EdgeOfTimeRecomp
Comment=Spider-Man: Edge of Time, recompiled for PC
Exec=EdgeOfTimeRecomp
Icon=EdgeOfTimeRecomp
Terminal=false
Categories=Game;
DESKTOP
cp res/icon/reeot_icon.png "${APPDIR}/${APP_NAME}.png"
cp res/icon/reeot_icon.png "${APPDIR}/.DirIcon"

tool_dir="${XDG_CACHE_HOME:-$HOME/.cache}/reeot"
tool="${tool_dir}/appimagetool-${APPIMAGE_ARCH}.AppImage"
if [ ! -x "${tool}" ]; then
    mkdir -p "${tool_dir}"
    echo "fetching appimagetool-${APPIMAGE_ARCH}..."
    curl -fsSL -o "${tool}" \
        "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-${APPIMAGE_ARCH}.AppImage"
    chmod +x "${tool}"
fi
export APPIMAGE_EXTRACT_AND_RUN=1

mkdir -p dist
rm -f dist/"${APP_NAME}"-*.AppImage
ARCH="${APPIMAGE_ARCH}" "${tool}" "${APPDIR}" "dist/${OUT_FILE}"

if [ -d "${DOWNLOADS}" ]; then
    rm -f "${DOWNLOADS}"/"${APP_NAME}"-*.AppImage
    cp "dist/${OUT_FILE}" "${DOWNLOADS}/"
    echo "Packaged dist/${OUT_FILE} ($(du -m "dist/${OUT_FILE}" | cut -f1) MB), copied to ${DOWNLOADS}"
else
    echo "Packaged dist/${OUT_FILE} ($(du -m "dist/${OUT_FILE}" | cut -f1) MB)"
fi
