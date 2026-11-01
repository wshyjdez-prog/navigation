#!/usr/bin/env bash
set -euo pipefail
cd /home/navigation/p1-assignment2-20261101-member1-01
sha256sum source.tar.gz > final-package-hashes.log
tar -xzf source.tar.gz -C current
(cd current && sha256sum -c ../source-files.sha256) > source-verification.log
cmake --build build -j1 > build.log 2>&1
ctest --test-dir build --output-on-failure > ctest.log 2>&1
QT_QPA_PLATFORM=offscreen ./build/p1-navigation --smoke > smoke.log 2>&1
QT_QPA_PLATFORM=offscreen ./build/p1-navigation --capture-dir captures > capture.log 2>&1
cmake --install build --prefix "$PWD/install" > install.log 2>&1
QT_QPA_PLATFORM=offscreen ./install/bin/p1-navigation --smoke > installed-smoke.log 2>&1
bash verify-desktop.sh
cp build/Testing/Temporary/LastTest.log LastTest.log
ldd install/bin/p1-navigation > ldd.log
if grep -q 'not found' ldd.log; then cat ldd.log; exit 1; fi
tar -czf p1-navigation-assignment2-linux-amd64.tar.gz -C install .
sha256sum install/bin/p1-navigation p1-navigation-assignment2-linux-amd64.tar.gz >> final-package-hashes.log
cat ctest.log final-package-hashes.log
printf 'Compiler warnings after visual correction: '
grep -c 'warning:' build.log || true
printf 'Source files verified: '
wc -l < source-verification.log
