#!/usr/bin/env bash
set -euo pipefail

VERSION=${VERSION:?VERSION is required}
TARGET=${TARGET:?TARGET is required}
DIST=${DIST:-dist}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)

cd "$ROOT"

name="heddle-${VERSION}-${TARGET}"
stage="${DIST}/${name}"
mkdir -p "$stage/bin"

find_bin() {
    local stem=$1
    local hit
    for hit in build/*/*/release/"$stem" build/*/*/release/"$stem".exe; do
        [ -f "$hit" ] && { printf '%s' "$hit"; return 0; }
    done
    return 1
}

heddle_bin=$(find_bin heddle) || { echo "package: heddle not found" >&2; exit 1; }
loom_bin=$(find_bin loom) || { echo "package: loom not found" >&2; exit 1; }

cp "$heddle_bin" "$stage/bin/"
cp "$loom_bin" "$stage/bin/"

cp README.md LICENSE "$stage/"
printf '%s\n' "$VERSION" > "$stage/VERSION"

cat > "$stage/BUILD.txt" <<EOF
version: ${VERSION}
target:  ${TARGET}
commit:  ${COMMIT:-unknown}
date:    ${BUILD_DATE:-unknown}
EOF

if [ "${TARGET%%-*}" = windows ]; then
    command -v zip >/dev/null || { echo "package: zip not found" >&2; exit 1; }
    ( cd "$DIST" && zip -qr "${name}.zip" "$name" )
else
    tar -czf "${DIST}/${name}.tar.gz" -C "$DIST" "$name"
fi

cat > "${DIST}/${name}.json" <<EOF
{
  "name": "heddle",
  "version": "${VERSION}",
  "target": "${TARGET}",
  "commit": "${COMMIT:-unknown}",
  "build_date": "${BUILD_DATE:-unknown}",
  "binaries": ["heddle", "loom"]
}
EOF

rm -rf "$stage"
echo "package: ${DIST}/${name}"
