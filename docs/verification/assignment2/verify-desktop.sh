#!/usr/bin/env bash
set -euo pipefail
cd /home/navigation/p1-assignment2-20261101-member1-01
export XDG_RUNTIME_DIR=/run/user/1000
export WAYLAND_DISPLAY=wayland-0
test -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY"
export QT_QPA_PLATFORM=wayland
./build/p1-ui-tests > wayland-ui.log 2>&1
./install/bin/p1-navigation --smoke > wayland-smoke.log 2>&1
./build/p1-navigation --capture-dir wayland-captures > wayland-capture.log 2>&1
cat wayland-ui.log
cmake -S current -B build-no-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/g++-13 -DBUILD_TESTING=OFF > no-tests-configure.log 2>&1
cmake --build build-no-tests -j1 > no-tests-build.log 2>&1
ctest --test-dir build-no-tests -N > no-tests-ctest.log 2>&1
cat no-tests-ctest.log
printf 'BUILD_TESTING=OFF compiler warnings: '
grep -c 'warning:' no-tests-build.log || true
printf 'DESKTOP VERIFICATION COMPLETE\n'
