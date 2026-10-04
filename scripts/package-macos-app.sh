#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

PRESET="${1:-mac-arm64-release}"
BUILD_DIR="out/build/${PRESET}"
SIGN_IDENTITY="${SIGN_IDENTITY:--}"
MAX_MACOS="${MAX_MACOS:-13.3}"
DOWNLOADS="${HOME}/Downloads"
BUNDLE_ID="${BUNDLE_ID:-com.graine25.reeot}"
APP_NAME="EdgeOfTimeRecomp"

if [ ! -x "${BUILD_DIR}/${APP_NAME}" ]; then
    echo "[FAILED] No build at ${BUILD_DIR}. Build first (scripts/build.sh), then run this." >&2
    exit 1
fi
if [ ! -f "${BUILD_DIR}/program_files.txt" ]; then
    echo "[FAILED] ${BUILD_DIR}/program_files.txt is missing; cannot tell what to package." >&2
    exit 1
fi
if [ ! -f "${BUILD_DIR}/vulkan/lib/libMoltenVK.dylib" ]; then
    echo "[FAILED] ${BUILD_DIR}/vulkan/lib holds no MoltenVK; the SDK stages it on every link." >&2
    exit 1
fi

version="$(grep -m1 'REEOT_VERSION_STRING' "${BUILD_DIR}/generated/core/build_info.h" | sed -n 's/.*"\([^"]*\)".*/\1/p')"
stamp="$(sed -n '1p' "${BUILD_DIR}/build_stamp.txt" 2>/dev/null | tr -d '\r')"
archs="$(lipo -archs "${BUILD_DIR}/${APP_NAME}")"
arch_tag="$(echo "${archs}" | tr ' ' '-')"
OUT_BASE="${APP_NAME}-v${version:-unknown}${stamp:+-$stamp}-mac-${arch_tag}"
ZIP_FILE="${OUT_BASE}.zip"
DMG_FILE="${OUT_BASE}.dmg"

version_gt() {
    awk -v a="$1" -v b="$2" 'BEGIN {
        split(a, av, "."); split(b, bv, ".");
        for (i = 1; i <= 4; ++i) {
            ai = av[i] + 0; bi = bv[i] + 0;
            if (ai > bi) exit 0;
            if (ai < bi) exit 1;
        }
        exit 1;
    }'
}

STAGING="$(mktemp -d "${TMPDIR:-/tmp}/${APP_NAME}-macos-package.XXXXXX")"
trap 'rm -rf "${STAGING}"' EXIT

APP_BUNDLE="${STAGING}/${APP_NAME}.app"
CONTENTS="${APP_BUNDLE}/Contents"
MACOS_DIR="${CONTENTS}/MacOS"
RESOURCES_DIR="${CONTENTS}/Resources"
mkdir -p "${MACOS_DIR}" "${RESOURCES_DIR}"

while IFS= read -r f; do
    f="${f%$'\r'}"
    [ -n "$f" ] || continue
    if [ ! -e "${BUILD_DIR}/${f}" ]; then
        echo "[FAILED] ${f} is listed in program_files.txt but not in ${BUILD_DIR}." >&2
        exit 1
    fi
    case "${f}" in
        vulkan/*) dest="${RESOURCES_DIR}" ;;
        *) if file "${BUILD_DIR}/${f}" | grep -q 'Mach-O'; then dest="${MACOS_DIR}"; else dest="${RESOURCES_DIR}"; fi ;;
    esac
    mkdir -p "${dest}/$(dirname "${f}")"
    cp "${BUILD_DIR}/${f}" "${dest}/${f}"
    chmod u+w "${dest}/${f}"
done < "${BUILD_DIR}/program_files.txt"
cp "${BUILD_DIR}/program_files.txt" "${RESOURCES_DIR}/"
chmod 755 "${MACOS_DIR}/${APP_NAME}"

macho_files=()
while IFS= read -r -d '' candidate; do
    if file "${candidate}" | grep -q 'Mach-O'; then
        macho_files+=("${candidate}")
    fi
done < <(find "${CONTENTS}" -type f -print0)

macho_loads() {
    otool -l "$1" | awk '
        $1 == "cmd" && ($2 == "LC_LOAD_DYLIB" ||
                        $2 == "LC_LOAD_WEAK_DYLIB" ||
                        $2 == "LC_REEXPORT_DYLIB" ||
                        $2 == "LC_LOAD_UPWARD_DYLIB") { want_name = 1; next }
        want_name && $1 == "name" { print $2; want_name = 0 }
    '
}
dependency_errors=()
for binary in "${macho_files[@]}"; do
    while IFS= read -r dep; do
        case "${dep}" in
            /System/Library/*|/usr/lib/*) ;;
            @rpath/*|@executable_path/*)
                [ -e "${MACOS_DIR}/${dep#*/}" ] || [ -e "$(dirname "${binary}")/${dep#*/}" ] ||
                    dependency_errors+=("${binary#${APP_BUNDLE}/}: unresolved ${dep}") ;;
            @loader_path/*)
                [ -e "$(dirname "${binary}")/${dep#@loader_path/}" ] ||
                    dependency_errors+=("${binary#${APP_BUNDLE}/}: unresolved ${dep}") ;;
            *) dependency_errors+=("${binary#${APP_BUNDLE}/}: host dependency ${dep}") ;;
        esac
    done < <(macho_loads "${binary}")
done
if [ ${#dependency_errors[@]} -gt 0 ]; then
    printf '[FAILED] %s\n' "${dependency_errors[@]}" >&2
    echo "refusing to package: the application is not self-contained." >&2
    exit 1
fi

minimum_macos() {
    otool -l "$1" | awk '
        $1 == "cmd" && $2 == "LC_BUILD_VERSION" { build = 1; legacy = 0; next }
        $1 == "cmd" && $2 == "LC_VERSION_MIN_MACOSX" { legacy = 1; build = 0; next }
        build && $1 == "minos" { print $2; exit }
        legacy && $1 == "version" { print $2; exit }
    '
}
package_min_macos=0.0
violations=()
for binary in "${macho_files[@]}"; do
    minos="$(minimum_macos "${binary}")"
    if [ -z "${minos}" ]; then
        echo "[FAILED] could not read the macOS deployment target from ${binary#${APP_BUNDLE}/}" >&2
        exit 1
    fi
    if version_gt "${minos}" "${package_min_macos}"; then
        package_min_macos="${minos}"
    fi
    if version_gt "${minos}" "${MAX_MACOS}"; then
        violations+=("${binary#${APP_BUNDLE}/} requires macOS ${minos} (ceiling ${MAX_MACOS})")
    fi
    for arch in ${archs}; do
        if ! lipo "${binary}" -verify_arch "${arch}" >/dev/null 2>&1; then
            echo "[FAILED] ${binary#${APP_BUNDLE}/} does not contain ${arch}" >&2
            exit 1
        fi
    done
done
if [ ${#violations[@]} -gt 0 ]; then
    printf '[FAILED] %s\n' "${violations[@]}" >&2
    echo "refusing to package: these would fail to start on the target systems." >&2
    echo "Rebuild with CMAKE_OSX_DEPLOYMENT_TARGET=${MAX_MACOS} or lower (the mac presets set it)," >&2
    echo "or raise the ceiling (MAX_MACOS) only for a package meant for hosts this new." >&2
    exit 1
fi

ICONSET="${STAGING}/${APP_NAME}.iconset"
mkdir -p "${ICONSET}"
make_icon() {
    sips -z "$1" "$1" res/app/icon/reeot_icon.png --out "${ICONSET}/$2" >/dev/null
}
make_icon 16 icon_16x16.png
make_icon 32 icon_16x16@2x.png
make_icon 32 icon_32x32.png
make_icon 64 icon_32x32@2x.png
make_icon 128 icon_128x128.png
make_icon 256 icon_128x128@2x.png
make_icon 256 icon_256x256.png
make_icon 512 icon_256x256@2x.png
make_icon 512 icon_512x512.png
make_icon 1024 icon_512x512@2x.png
if ! iconutil -c icns "${ICONSET}" -o "${RESOURCES_DIR}/${APP_NAME}.icns" 2>/dev/null; then
    echo "[WARN] iconutil rejected the iconset; using the PNG as the one icon"
    sips -s format icns res/app/icon/reeot_icon.png --out "${RESOURCES_DIR}/${APP_NAME}.icns" >/dev/null
fi

cat > "${CONTENTS}/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleDisplayName</key>
    <string>${APP_NAME}</string>
    <key>CFBundleExecutable</key>
    <string>${APP_NAME}</string>
    <key>CFBundleIconFile</key>
    <string>${APP_NAME}.icns</string>
    <key>CFBundleIdentifier</key>
    <string>${BUNDLE_ID}</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>${APP_NAME}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>${version:-0.0}</string>
    <key>CFBundleVersion</key>
    <string>${version:-0.0}</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.games</string>
    <key>LSMinimumSystemVersion</key>
    <string>${package_min_macos}</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
PLIST
plutil -lint "${CONTENTS}/Info.plist" >/dev/null

xattr -cr "${APP_BUNDLE}"
find "${APP_BUNDLE}" -name '._*' -delete
sign_args=(--force --sign "${SIGN_IDENTITY}")
if [ "${SIGN_IDENTITY}" != - ]; then
    sign_args+=(--options runtime --timestamp)
fi
for binary in "${macho_files[@]}"; do
    if [ "${binary}" != "${MACOS_DIR}/${APP_NAME}" ]; then
        codesign "${sign_args[@]}" "${binary}"
    fi
done
codesign "${sign_args[@]}" --entitlements res/app/macos/reeot.entitlements "${MACOS_DIR}/${APP_NAME}"
codesign "${sign_args[@]}" --entitlements res/app/macos/reeot.entitlements "${APP_BUNDLE}"
codesign --verify --deep --strict "${APP_BUNDLE}"

mkdir -p dist
rm -f dist/"${APP_NAME}"-*-mac-*.zip dist/"${APP_NAME}"-*-mac-*.dmg
ditto -c -k --sequesterRsrc --keepParent "${APP_BUNDLE}" "dist/${ZIP_FILE}"

DMG_STAGING="${STAGING}/dmg"
mkdir -p "${DMG_STAGING}"
mv "${APP_BUNDLE}" "${DMG_STAGING}/${APP_NAME}.app"
ln -s /Applications "${DMG_STAGING}/Applications"
hdiutil create -quiet -ov -volname "${APP_NAME} ${version:-}" -srcfolder "${DMG_STAGING}" \
    -fs HFS+ -format UDZO -imagekey zlib-level=9 "dist/${DMG_FILE}"
if [ "${SIGN_IDENTITY}" != - ]; then
    codesign --force --sign "${SIGN_IDENTITY}" --timestamp "dist/${DMG_FILE}"
fi

if [ -d "${DOWNLOADS}" ]; then
    rm -f "${DOWNLOADS}"/"${APP_NAME}"-*-mac-*.zip "${DOWNLOADS}"/"${APP_NAME}"-*-mac-*.dmg
    cp "dist/${ZIP_FILE}" "dist/${DMG_FILE}" "${DOWNLOADS}/"
    echo "Packaged dist/${ZIP_FILE} ($(du -m "dist/${ZIP_FILE}" | cut -f1) MB; minimum macOS ${package_min_macos}), copied to ${DOWNLOADS}"
    echo "Packaged dist/${DMG_FILE} ($(du -m "dist/${DMG_FILE}" | cut -f1) MB), copied to ${DOWNLOADS}"
else
    echo "Packaged dist/${ZIP_FILE} ($(du -m "dist/${ZIP_FILE}" | cut -f1) MB; minimum macOS ${package_min_macos})"
    echo "Packaged dist/${DMG_FILE} ($(du -m "dist/${DMG_FILE}" | cut -f1) MB)"
fi
