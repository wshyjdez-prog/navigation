"""Post-measurement validation/deployment; preserves existing datasets."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import sqlite3
import subprocess

base = Path('/home/navigation/p1-import-libosmium-20261101')
out = base / 'evidence'
record = json.loads((out / 'hubei.json').read_text())
db = sqlite3.connect('file:'+record['cachePath']+'?mode=ro', uri=True)
summary = json.loads(db.execute("select value from metadata where key='summary'").fetchone()[0])
check = {'integrity': db.execute('pragma integrity_check').fetchone()[0],
         'counts': {t:db.execute('select count(*) from '+t).fetchone()[0]
                    for t in ['nodes','ways','relations','way_nodes','relation_members']},
         'summary':summary, 'cacheBytes':Path(record['cachePath']).stat().st_size}
assert check['integrity']=='ok'
assert check['counts']['nodes']==9077243 and check['counts']['ways']==593776
assert check['counts']['relations']==5262
db.close()
(out / 'hubei-validation.json').write_text(json.dumps(check,indent=2,ensure_ascii=False))
navit = Path('/home/navigation/Applications/Navit')
baseline = {str(p.relative_to(navit)):hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
            for p in [navit/'navit.xml',navit/'root/usr/bin/maptool']}
(out / 'navit-hashes.json').write_text(json.dumps(baseline,indent=2))
subprocess.run(['cmake','--install',str(base/'release-final'),'--prefix',str(base/'install')],check=True)
licenses = base / 'install/share/p1-navigation/licenses'
licenses.mkdir(parents=True,exist_ok=True)
for root in [Path('/home/navigation/p1-deps/osmium-2.20/root/usr/share/doc'),Path('/usr/share/doc')]:
    for package in ['libosmium2-dev','libprotozero-dev','libexpat1-dev','zlib1g-dev','libbz2-1.0','libsqlite3-0']:
        source = root/package/'copyright'
        if source.is_file(): shutil.copy(source,licenses/(package+'-copyright'))
old = '/home/navigation/p1-assignment2-20261101-member1-01/install'
for desktop in [Path('/home/navigation/Desktop/P1-Navigation.desktop'),
                Path('/home/navigation/.local/share/applications/p1-navigation.desktop')]:
    text = desktop.read_text()
    assert old in text
    desktop.write_text(text.replace(old,str(base/'install')))
    desktop.chmod(0o755)
env = dict(os.environ,XDG_RUNTIME_DIR='/run/user/1000',
           DBUS_SESSION_BUS_ADDRESS='unix:path=/run/user/1000/bus',
           WAYLAND_DISPLAY='wayland-0',QT_QPA_PLATFORM='wayland')
binary = str(base/'install/bin/p1-navigation')
with (out/'wayland-ui.txt').open('w') as log:
    subprocess.run([str(base/'release-final/p1-ui-tests')],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
with (out/'capture.txt').open('w') as log:
    subprocess.run([binary,'--capture-dir',str(out/'captures')],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
with (out/'installed-app.txt').open('w') as log:
    subprocess.Popen([binary],env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
print(json.dumps(check,ensure_ascii=False))
