#!/usr/bin/env bash
# 1 号：准备 LinuxVoiceAdapter 的第三方依赖与离线语音资源（不进 Git，按 SHA256 校验）。
# 结果目录 third_party/voice/：
#   vosk/vosk-linux-x86_64-0.3.45/   Vosk 0.3.45 预编译库（Apache-2.0）
#   resources/                       随应用分发的离线资源 → 安装到 share/p1-navigation/voice
#     vosk-model-small-cn-0.22/      Vosk 普通话小模型（Apache-2.0）
#     piper/                         Piper 2023.11.14-2 运行时（MIT；内含 espeak-ng GPL-3.0、onnxruntime MIT）
#     piper-voice/                   zh_CN-huayan-medium 音色（许可见 rhasspy/piper-voices MODEL_CARD，未核实）
# 系统包（需 sudo，由用户执行）：sudo apt install libpulse-dev unzip
# 只含最终方案 Piper + Vosk 的依赖（DEC-20261123-1-voice-backend-01）。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party/voice"
DL="$TP/downloads"
mkdir -p "$DL" "$TP/resources"

fetch() {  # 目录 文件名 URL SHA256
  local dir="$DL/$1" file="$2" url="$3" sum="$4"
  mkdir -p "$dir"
  if [[ ! -f "$dir/$file" ]] || ! echo "$sum  $dir/$file" | sha256sum -c --quiet - 2>/dev/null; then
    echo "下载 $file"
    # WSL 的部分 IPv6 路由会停在 0 字节后报 curl(56)；固定 IPv4 并允许
    # 网络错误重试，避免每次都从用户目录重新判断问题。
    curl --ipv4 -fL --retry 5 --retry-all-errors --retry-delay 2 \
      --connect-timeout 30 -o "$dir/$file.part" "$url"
    mv "$dir/$file.part" "$dir/$file"
  fi
  echo "$sum  $dir/$file" | sha256sum -c --quiet - || { echo "校验失败: $file" >&2; exit 1; }
}

fetch vosk-lib vosk-linux-x86_64-0.3.45.zip \
  https://github.com/alphacep/vosk-api/releases/download/v0.3.45/vosk-linux-x86_64-0.3.45.zip \
  bbdc8ed85c43979f6443142889770ea95cbfbc56cffb5c5dcd73afa875c5fbb2
fetch vosk-model-cn vosk-model-small-cn-0.22.zip \
  https://alphacephei.com/vosk/models/vosk-model-small-cn-0.22.zip \
  3af8b0e7e0f835ae9d414ce5df580237a3cfb08d586c9fbbb0f7ff29ad5b14ba
fetch piper piper_linux_x86_64.tar.gz \
  https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_linux_x86_64.tar.gz \
  a50cb45f355b7af1f6d758c1b360717877ba0a398cc8cbe6d2a7a3a26e225992
fetch piper-voice zh_CN-huayan-medium.onnx \
  https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/zh/zh_CN/huayan/medium/zh_CN-huayan-medium.onnx \
  9929917bf8cabb26fd528ea44d3a6699c11e87317a14765312420be230be0f3d
fetch piper-voice zh_CN-huayan-medium.onnx.json \
  https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/zh/zh_CN/huayan/medium/zh_CN-huayan-medium.onnx.json \
  d521dc45504a8ccc99e325822b35946dd701840bfb07e3dbb31a40929ed6a82b

# 解压到各自目录（下载内容视为不可信数据，只解压不执行其中脚本）
if [[ ! -f "$TP/vosk/vosk-linux-x86_64-0.3.45/libvosk.so" ]]; then
  mkdir -p "$TP/vosk" && (cd "$TP/vosk" && unzip -q -o "$DL/vosk-lib/vosk-linux-x86_64-0.3.45.zip")
fi
if [[ ! -d "$TP/resources/vosk-model-small-cn-0.22" ]]; then
  (cd "$TP/resources" && unzip -q -o "$DL/vosk-model-cn/vosk-model-small-cn-0.22.zip")
fi
if [[ ! -x "$TP/resources/piper/piper" ]]; then
  tar -xzf "$DL/piper/piper_linux_x86_64.tar.gz" -C "$TP/resources"
fi
mkdir -p "$TP/resources/piper-voice"
cp -u "$DL/piper-voice/"* "$TP/resources/piper-voice/"

echo "语音依赖就绪：$TP"
du -sh "$TP/resources"/* 2>/dev/null
