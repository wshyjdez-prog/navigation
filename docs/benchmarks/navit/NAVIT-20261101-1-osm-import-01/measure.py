"""Run on the unified Ubuntu VM; writes raw evidence beside this script."""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import tempfile

base = Path('/home/navigation/p1-import-libosmium-20261101')
out = base / 'evidence'
source = Path('/home/navigation/Applications/Navit/maps/hubei-261031.osm.pbf')
navit = Path('/home/navigation/Applications/Navit/root/usr')
env = dict(os.environ, LD_LIBRARY_PATH=str(navit / 'lib/x86_64-linux-gnu'))
info = {'uname': platform.uname()._asdict(), 'cpu': os.cpu_count(),
        'meminfo': Path('/proc/meminfo').read_text(), 'inputBytes': source.stat().st_size,
        'inputSha256': hashlib.file_digest(source.open('rb'), 'sha256').hexdigest()}
for name, cmd in {'compiler':['/usr/bin/g++-13','--version'], 'cmake':['cmake','--version'],
                  'qt':['qmake6','-query','QT_VERSION'], 'processes':['ps','-eo','comm,pcpu,rss']}.items():
    info[name] = subprocess.run(cmd, capture_output=True, text=True).stdout
(out / 'environment.json').write_text(json.dumps(info, indent=2))
rows = []
for tool in ['p1', 'navit']:
    for iteration in range(4):
        with tempfile.TemporaryDirectory(prefix='benchmark-', dir=base) as temp:
            target = Path(temp)
            cmd = ([str(base / 'release-final/src/data/import/p1-import-cli'),str(source),str(target / 'cache')]
                   if tool == 'p1' else
                   [str(navit / 'bin/maptool'),'--protobuf','--threads','1','--slice-size',
                    '268435456','--unknown-country','-i',str(source),str(target / 'map.bin')])
            stem = out / f'{tool}-{iteration}'
            with stem.with_suffix('.stdout').open('w') as stdout, stem.with_suffix('.stderr').open('w') as stderr:
                try:
                    proc = subprocess.run(['/usr/bin/time','-f','%e %M','-o',str(stem.with_suffix('.time'))]+cmd,
                                          cwd=temp, env=env, stdout=stdout, stderr=stderr, timeout=600)
                    code = proc.returncode
                except subprocess.TimeoutExpired:
                    code = 'timeout'
            timing = stem.with_suffix('.time').read_text().strip().splitlines()[-1].split()
            row = dict(tool=tool, iteration=iteration, warmup=iteration==0, exit=code,
                       seconds=float(timing[0]), peakMiB=int(timing[1])/1024,
                       outputBytes=sum(p.stat().st_size for p in target.rglob('*') if p.is_file()), command=cmd)
            if tool == 'p1' and code == 0:
                row['result'] = json.loads(stem.with_suffix('.stdout').read_text())
            rows.append(row)
            (out / 'raw.json').write_text(json.dumps(rows, indent=2))
            print(tool, iteration, row['seconds'], row['peakMiB'], code, flush=True)
summary = {}
for tool in ['p1','navit']:
    samples = [r for r in rows if r['tool']==tool and not r['warmup'] and r['exit']==0]
    times = sorted(r['seconds'] for r in samples)
    summary[tool] = {'n':len(samples), 'medianSeconds':statistics.median(times),
                     'p95Seconds':times[-1], 'maxPeakMiB':max(r['peakMiB'] for r in samples),
                     'medianInputMiBps':source.stat().st_size/1048576/statistics.median(times)}
(out / 'summary.json').write_text(json.dumps(summary, indent=2))
