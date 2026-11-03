#!/usr/bin/env bash
# Ubuntu 24.04 verification helper for the 2 号 "unified data" module.
#
# It builds the repository with the team toolchain, runs the OSMX and import-page
# test targets, and optionally parses a real .osmx file while recording time and
# peak memory. It never edits source files and never writes outside the build
# directory.
#
# Usage:
#   src/data/osmx/tools/verify-osmx.sh [options]
#
# Options:
#   -s, --source DIR     repository root (default: derived from this script)
#   -b, --build DIR      build directory (default: <repo>/build-osmx)
#   -f, --file FILE      real .osmx file for the end-to-end check (optional)
#   -j, --jobs N         parallel build jobs (default: nproc)
#   -h, --help           show this help
#
# Exit code: 0 only when configure, build, tests and the optional real-file check
# all succeed.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_dir="$(cd "${script_dir}/../../../.." && pwd)"
build_dir=""
real_file=""
jobs="$(nproc 2>/dev/null || echo 2)"

usage() {
    sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -s|--source) source_dir="$2"; shift 2 ;;
        -b|--build)  build_dir="$2"; shift 2 ;;
        -f|--file)   real_file="$2"; shift 2 ;;
        -j|--jobs)   jobs="$2"; shift 2 ;;
        -h|--help)   usage ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "${build_dir}" ]]; then
    build_dir="${source_dir}/build-osmx"
fi

log_file="${build_dir}/verify-osmx.log"
mkdir -p "${build_dir}"
exec > >(tee "${log_file}") 2>&1

echo "== P1 OSMX verification =="
echo "date:        $(date -Is)"
echo "host:        $(uname -srm)"
echo "source:      ${source_dir}"
echo "build:       ${build_dir}"
echo "jobs:        ${jobs}"

echo
echo "== toolchain =="
cmake --version | head -n 1
c++ --version | head -n 1 || true
g++-13 --version | head -n 1 || true
ninja --version 2>/dev/null || echo "ninja: not found (CMake will pick another generator)"
if command -v qmake6 >/dev/null 2>&1; then
    qmake6 -query QT_VERSION | sed 's/^/Qt: /'
elif command -v qtpaths6 >/dev/null 2>&1; then
    qtpaths6 --qt-version | sed 's/^/Qt: /'
else
    echo "Qt: qmake6/qtpaths6 not found; CMake must find Qt 6.4.2 itself"
fi

echo
echo "== configure =="
cmake -S "${source_dir}" -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_COMPILER="${CXX:-/usr/bin/g++-13}"

echo
echo "== build =="
cmake --build "${build_dir}" -j "${jobs}"

echo
echo "== tests (data-osmx, import-page-osmx) =="
ctest --test-dir "${build_dir}" --output-on-failure -R 'data-osmx|import-page-osmx'

if [[ -n "${real_file}" ]]; then
    if [[ ! -f "${real_file}" ]]; then
        echo "real file not found: ${real_file}" >&2
        exit 1
    fi
    echo
    echo "== real file: ${real_file} =="
    real_file="$(cd "$(dirname "${real_file}")" && pwd)/$(basename "${real_file}")"
    sha256sum "${real_file}" | sed 's/^/sha256: /'

    cli="${build_dir}/src/data/osmx/p1-osmx-cli"
    echo "-- header check --"
    "${cli}" "${real_file}" --header-only

    echo "-- full parse (time -v) --"
    if command -v /usr/bin/time >/dev/null 2>&1; then
        /usr/bin/time -v "${cli}" "${real_file}"
    else
        "${cli}" "${real_file}"
    fi

    echo "-- real-file unit test --"
    P1_OSMX_REAL_FILE="${real_file}" "${build_dir}/src/data/osmx/p1-osmx-tests"
else
    echo
    echo "== real file check skipped (pass --file /path/test.osmx to enable) =="
fi

echo
echo "== done: all checks passed =="
echo "log: ${log_file}"
