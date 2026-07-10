#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT}/build-switch}"
CMAKE_BIN="${CMAKE_BIN:-cmake}"
PIPENSX_METADATA_INDEX="${PIPENSX_METADATA_INDEX:-${ROOT}/resources/catalog/game_metadata_index.json}"
PIPENSX_VERSION="${PIPENSX_VERSION:-0.0.0}"

if [[ -z "${DEVKITPRO:-}" ]]; then
    echo "DEVKITPRO is not set." >&2
    exit 1
fi

for tool in "${CMAKE_BIN}" "${DEVKITPRO}/tools/bin/uam" \
    "${DEVKITPRO}/tools/bin/nacptool" "${DEVKITPRO}/tools/bin/elf2nro"; do
    if [[ "${tool}" == */* ]]; then
        [[ -x "${tool}" ]] || { echo "Missing tool: ${tool}" >&2; exit 1; }
    else
        command -v "${tool}" >/dev/null ||
            { echo "Missing tool: ${tool}" >&2; exit 1; }
    fi
done

for library in curl mbedtls mbedx509 mbedcrypto z miniupnpc; do
    [[ -f "${DEVKITPRO}/portlibs/switch/lib/lib${library}.a" ]] ||
        { echo "Missing Switch library: lib${library}.a" >&2; exit 1; }
done
[[ -f "${DEVKITPRO}/libnx/lib/libdeko3d.a" ]] ||
    { echo "Missing deko3d from libnx." >&2; exit 1; }
[[ -f "${ROOT}/vendor/glm/glm/vec2.hpp" ]] ||
    { echo "Missing GLM submodule. Run: git submodule update --init --recursive" >&2; exit 1; }
[[ -f "${ROOT}/vendor/borealis/library/CMakeLists.txt" ]] ||
    { echo "Missing Borealis submodule. Run: git submodule update --init --recursive" >&2; exit 1; }

"${CMAKE_BIN}" -S "${ROOT}" -B "${BUILD_DIR}" \
    -DPLATFORM_SWITCH=ON \
    -DUSE_DEKO3D=ON \
    "-DPIPENSX_VERSION=${PIPENSX_VERSION}" \
    "-DPIPENSX_METADATA_INDEX=${PIPENSX_METADATA_INDEX}" \
    -DCMAKE_BUILD_TYPE=Release
"${CMAKE_BIN}" --build "${BUILD_DIR}" --target pipensx.nro --parallel

echo "Built: ${BUILD_DIR}/pipensx.nro"
