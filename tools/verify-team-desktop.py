"""Post-build Ubuntu/Wayland validation. Run after the root CTest suite passes.

Usage: python3 tools/verify-team-desktop.py /absolute/verification/workspace
The workspace must contain build/, source/ and evidence/.
"""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys

base = Path(sys.argv[1]).resolve()
build = base / 'build'
evidence = base / 'evidence'
evidence.mkdir(exist_ok=True)
env = dict(os.environ, XDG_RUNTIME_DIR='/run/user/1000',
           DBUS_SESSION_BUS_ADDRESS='unix:path=/run/user/1000/bus',
           WAYLAND_DISPLAY='wayland-0', QT_QPA_PLATFORM='wayland',
           P1_UI_CAPTURE_DIR=str(evidence / 'captures'))

def run(name, args, environment=None):
    with (evidence / name).open('w') as log:
        result = subprocess.run(args, env=environment, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f'{name}: exit {result.returncode}; see evidence')

run('wayland-ui.txt', [str(build/'p1-ui-tests')], env)
run('wayland-osmx-page.txt', [str(build/'src/app/p1-import-page-tests')], env)
run('capture.txt', [str(build/'p1-navigation'), '--capture-dir', str(evidence/'captures')], env)
prefix = base / 'install'
run('install.txt', ['cmake', '--install', str(build), '--prefix', str(prefix)])
binary = str(prefix/'bin/p1-navigation')
run('installed-smoke.txt', [binary, '--smoke'], env)
run('ldd.txt', ['ldd', binary])
assert 'not found' not in (evidence/'ldd.txt').read_text()

licenses = prefix/'share/p1-navigation/licenses'
licenses.mkdir(parents=True, exist_ok=True)
old_licenses = Path('/home/navigation/p1-import-libosmium-20261101/install/share/p1-navigation/licenses')
if old_licenses.is_dir():
    shutil.copytree(old_licenses, licenses, dirs_exist_ok=True)
boost_copyright = Path('/home/navigation/p1-deps/boost-1.83/root/usr/share/doc/libboost1.83-dev/copyright')
if boost_copyright.exists():
    shutil.copy(boost_copyright, licenses/'boost-copyright')

updated = []
for path in [Path('/home/navigation/Desktop/P1-Navigation.desktop'),
             Path('/home/navigation/.local/share/applications/p1-navigation.desktop')]:
    if not path.is_file():
        continue
    text = path.read_text()
    if 'p1-navigation' not in text:
        raise RuntimeError(f'Unexpected desktop entry: {path}')
    fields = {'Exec':binary, 'TryExec':binary, 'Path':str(prefix)}
    lines = [line.split('=',1)[0]+'='+fields[line.split('=',1)[0]]
             if line.split('=',1)[0] in fields else line for line in text.splitlines()]
    path.write_text('\n'.join(lines)+'\n')
    path.chmod(0o755)
    updated.append(str(path))
(evidence/'deployment.json').write_text(json.dumps({'binary':binary,'shortcuts':updated},indent=2))
print('Wayland UI, OSMX page, captures, installation and runtime dependencies: PASS')
