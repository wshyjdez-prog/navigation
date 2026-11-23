#!/usr/bin/env bash
# NAVIT-20261123-1-voice-01：LinuxVoiceAdapter（Piper + Vosk）与 Navit 等价语音路径的实测。
# 三种候选组合的原始对比数据保留在报告目录；未选后端已删除（DEC-20261123-1-voice-backend-01），
# 本脚本只复测最终方案。
# 用法：tools/voice-bench.sh [构建目录] [输出目录]
# 前提：已按 tools/fetch-voice-deps.sh 准备 third_party/voice，构建目录含 p1-voice-bench。
# 运行期间不要有其他负载。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
OUT="${2:-$ROOT/docs/benchmarks/navit/NAVIT-20261123-1-voice-01}"
BENCH="$BUILD/src/pal/linux/voice/p1-voice-bench"
PHRASES="$ROOT/tests/fixtures/voice/nav_phrases_zh.txt"
RUNS="${RUNS:-3}"
WORK="$(mktemp -d /tmp/p1-voice-bench-XXXXXX)"
CORPUS="$WORK/corpus"
mkdir -p "$OUT" "$CORPUS"
trap 'rm -rf "$WORK"' EXIT

{
  echo "date: $(TZ=Asia/Shanghai date '+%F %T %Z')"
  echo "git: $(git -C "$ROOT" rev-parse --short HEAD) dirty=$(git -C "$ROOT" status --porcelain | wc -l)"
  uname -srm
  grep PRETTY_NAME /etc/os-release
  echo "virt: $(systemd-detect-virt 2>/dev/null || true)"
  lscpu | grep -E 'Model name|^CPU\(s\)|Thread'
  free -m | head -2
  echo "audio: $(pactl info 2>/dev/null | grep -E 'Server Name|Default Sink|Default Source' | tr '\n' ' ')"
  dpkg-query -W -f='${Package} ${Version}\n' speech-dispatcher speech-dispatcher-espeak-ng espeak-ng libpulse0 pipewire 2>/dev/null || true
  sha256sum "$PHRASES"
} > "$OUT/env.txt"

export LC_ALL=C.UTF-8
csv="$OUT/raw.csv"
: > "$csv"

echo "== ASR 输入：Piper 合成 20 句" | tee "$OUT/run.log"
"$BENCH" corpus --tts piper --phrases "$PHRASES" --out "$CORPUS" 2>&1 | tee -a "$OUT/run.log"
(cd "$CORPUS" && sha256sum *.wav) > "$OUT/corpus-sha256.txt"

echo "== TTS piper 实际播放（默认输出）" | tee -a "$OUT/run.log"
"$BENCH" tts --tts piper --phrases "$PHRASES" --runs "$RUNS" --csv "$csv" 2>&1 | tee -a "$OUT/run.log"
echo "== TTS piper 写 WAV（纯合成）" | tee -a "$OUT/run.log"
"$BENCH" tts --tts piper --phrases "$PHRASES" --runs "$RUNS" --csv "$csv" --wav 2>&1 | tee -a "$OUT/run.log"
echo "== ASR vosk 输入 piper 合成语音（尽快送入）" | tee -a "$OUT/run.log"
"$BENCH" asr --asr vosk --phrases "$PHRASES" --corpus "$CORPUS" --prefix piper --runs "$RUNS" --csv "$csv" 2>&1 | tee -a "$OUT/run.log"
echo "== ASR vosk 输入 piper 合成语音（实时送入，句末延迟）" | tee -a "$OUT/run.log"
"$BENCH" asr --asr vosk --phrases "$PHRASES" --corpus "$CORPUS" --prefix piper --runs 1 --csv "$csv" --realtime 2>&1 | tee -a "$OUT/run.log"

# Navit 0.5.7 speech_cmdline：把 data 中的 %s 替换为文本，spawn_process（fork/exec）执行，
# 新播报前阻塞等待上一条结束。这里用同样的 fork/exec 测量两种等价配置的“调用 → 命令结束”耗时。
echo "== Navit 0.5.7 speech_cmdline 等价路径" | tee -a "$OUT/run.log"
python3 -I - "$PHRASES" "$RUNS" "$OUT/navit.csv" <<'PY' 2>&1 | tee -a "$OUT/run.log"
import shutil, subprocess, sys, time, statistics, resource
phrases = [l.strip() for l in open(sys.argv[1], encoding="utf-8") if l.strip() and not l.startswith("#")]
runs = int(sys.argv[2])
configs = {
    "navit-cmdline-spd-say": ["spd-say", "-w", "-l", "zh", "-o", "espeak-ng", "%s"],
    "navit-cmdline-espeak-ng": ["espeak-ng", "-v", "cmn", "%s"],
}
subprocess.run(["spd-say", "-w", "-l", "zh", "预热"], check=False)
with open(sys.argv[3], "w", encoding="utf-8") as out:
    out.write("config,run,phrase,total_ms,chars\n")
    for name, argv in configs.items():
        if shutil.which(argv[0]) is None:
            print(f"  {name}: 未测（命令 {argv[0]} 未安装）")
            out.write(f"{name},,,untested-missing-{argv[0]},\n")
            continue
        totals, per_char = [], []
        for r in range(runs):
            for i, p in enumerate(phrases):
                cmd = [a.replace("%s", p) for a in argv]
                t0 = time.perf_counter()
                rc = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
                ms = (time.perf_counter() - t0) * 1000
                if rc != 0:
                    out.write(f"{name},{r},{i},fail,{len(p)}\n"); continue
                totals.append(ms); per_char.append(ms / len(p))
                out.write(f"{name},{r},{i},{ms:.2f},{len(p)}\n")
        q = statistics.quantiles(totals, n=20)
        print(f"  {name}: n={len(totals)} total_ms median={statistics.median(totals):.1f} p95={q[18]:.1f} "
              f"ms/char median={statistics.median(per_char):.1f}")
    ru = resource.getrusage(resource.RUSAGE_CHILDREN)
    print(f"  children max RSS (largest single child) = {ru.ru_maxrss/1024:.1f} MiB")
PY
echo "done: $OUT"
