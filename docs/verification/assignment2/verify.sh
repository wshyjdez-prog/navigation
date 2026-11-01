#!/usr/bin/env bash
set -euo pipefail
cd /home/navigation/p1-assignment2-20261101-member1-01
sha256sum baseline.tar source.tar.gz
mkdir baseline current
tar -xf baseline.tar -C baseline
tar -xzf source.tar.gz -C current
bash current/tools/verify-environment.sh
cmake -S baseline -B baseline-build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/g++-13 > baseline-configure.log 2>&1
cmake --build baseline-build --target p1-ui-tests -j1 > baseline-build.log 2>&1
set +e
ctest --test-dir baseline-build -R '^ui-contracts$' --output-on-failure > baseline-ui.log 2>&1
baseline_result=$?
set -e
printf 'Baseline ui-contracts exit=%s\n' "$baseline_result"
cat baseline-ui.log
cmake -S current -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/g++-13 > configure.log 2>&1
cmake --build build -j1 > build.log 2>&1
ctest --test-dir build --output-on-failure > ctest.log 2>&1
cat ctest.log
QT_QPA_PLATFORM=offscreen ./build/p1-navigation --smoke > smoke.log 2>&1
QT_QPA_PLATFORM=offscreen ./build/p1-navigation --capture-dir captures > capture.log 2>&1
cmake --install build --prefix "$PWD/install" > install.log 2>&1
QT_QPA_PLATFORM=offscreen ./install/bin/p1-navigation --smoke > installed-smoke.log 2>&1
ldd ./install/bin/p1-navigation > ldd.log
if grep -q 'not found' ldd.log; then cat ldd.log; exit 1; fi
printf 'Current compiler warnings: '
grep -c 'warning:' build.log || true
sha256sum install/bin/p1-navigation
printf 'VERIFICATION COMPLETE\n'
