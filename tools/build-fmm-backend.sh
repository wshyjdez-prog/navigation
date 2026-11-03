#!/bin/sh
# Build the approved external FMM commit without importing its global CMake
# settings into P1. Requires Ubuntu 24.04 GDAL/Boost Serialization development
# packages (or corresponding user-prefix CMAKE_PREFIX_PATH/CPATH/library paths).
# Usage: sh tools/build-fmm-backend.sh /path/to/external-prefix [pinned-source]
set -eu
prefix=${1:?Please supply an external installation prefix}
commit=19ef34e1f57ff2f2484231aa0d01dfffea986ec1
mkdir -p "$prefix"
prefix=$(cd "$prefix" && pwd)
if [ "$#" -ge 2 ]; then
    source_dir=$(cd "$2" && pwd)
    test "$(git -C "$source_dir" rev-parse HEAD)" = "$commit"
else
    source_dir="$prefix/source"
    if [ ! -d "$source_dir/.git" ]; then
        git init "$source_dir"
        git -C "$source_dir" fetch --depth 1 https://github.com/cyang-kth/fmm.git "$commit"
        git -C "$source_dir" checkout --detach FETCH_HEAD
    fi
    test "$(git -C "$source_dir" rev-parse HEAD)" = "$commit"
fi
# Keep upstream sources and licence files outside the repository. These are
# build-system compatibility changes; the HMM and UBODT algorithms are unchanged.
python3 - "$source_dir/CMakeLists.txt" <<'PY'
import pathlib,sys
p=pathlib.Path(sys.argv[1]); s=p.read_text()
s=s.replace('set(CMAKE_CXX_STANDARD 11)', 'set(CMAKE_CXX_STANDARD 17)')
s=s.replace('add_subdirectory(python)', '# P1: Python bindings disabled')
s=s.replace('include_directories(${FMM_INCLUDE_PATH})',
            'include_directories(${FMM_INCLUDE_PATH} ${Boost_INCLUDE_DIRS})')
p.write_text(s)
network=p.parent/'src/network/network.cpp'
s=network.read_text().replace('\n  OGRSpatialReference *ogrsr =', '\n  const OGRSpatialReference *ogrsr =')
network.write_text(s)
PY
cmake -S "$source_dir" -B "$prefix/build" -DCMAKE_CXX_COMPILER=/usr/bin/g++-13 \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DBoost_NO_BOOST_CMAKE=ON
cmake --build "$prefix/build" --target fmm ubodt_gen --parallel 1
mkdir -p "$prefix/bin" "$prefix/lib"
cp "$source_dir/build/fmm" "$source_dir/build/ubodt_gen" "$prefix/bin/"
cp "$prefix/build/libFMMLIB.so" "$prefix/lib/"
cp "$source_dir/LICENSE" "$prefix/"
printf 'FMM source: https://github.com/cyang-kth/fmm/tree/%s\n' "$commit"
printf 'Set P1_FMM_EXECUTABLE=%s/bin/fmm and P1_UBODT_EXECUTABLE=%s/bin/ubodt_gen\n' "$prefix" "$prefix"
printf 'Include %s/lib in LD_LIBRARY_PATH when required. Preserve upstream third_party licences.\n' "$prefix"
