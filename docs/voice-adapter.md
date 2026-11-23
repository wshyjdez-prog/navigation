# LinuxVoiceAdapter 设计说明（第 3 次作业 · 1 号）

最后更新：2026-11-23（Asia/Shanghai）。负责人：1 号。关联：`DEC-20261123-1-voice-backend-01`（后端组合对比，最终选定 **Piper 播报 + Vosk 识别**）、`NAVIT-20261123-1-voice-01`（Navit 对标）、X09（G10 语音：G10 未提供接口，按组员 1 要求以开源本地语音代替）。

## 1. 任务要求与实现范围

任务书第 3 次作业要求：`speak()` 调用本地 TTS、`listen()` 调用本地 ASR，设置中文语言环境，语音回调通过 FFI 传回核心层，注意语音服务生命周期；声明 MICROPHONE 权限，离线数据包随应用分发。

| 要求 | 实现 | 证据 |
| --- | --- | --- |
| speak() 本地 TTS | `p1_voice_speak()` → Piper（VITS 神经网络，onnxruntime，常驻子进程）中文音色 zh_CN-huayan-medium | `voice-adapter` 测试 `[tts piper]` |
| listen() 本地 ASR | `p1_voice_listen()` → Vosk 0.3.45（Kaldi）+ vosk-model-small-cn-0.22，流式、自带端点检测 | `voice-adapter` 测试 `[asr vosk]` |
| 中文语言环境 | 只接受 `zh*` 语言；普通话音色与普通话识别模型 | `p1_voice_create` 拒绝 `en-US` |
| 回调经 FFI 传回核心层 | 纯 C ABI `include/contracts/VoiceFfi.h`，`libp1_voice.so` 只导出 10 个 `p1_voice_*` 符号；纯 C 程序可直接使用 | `voice-ffi-c-smoke`（C99 程序）；`nm -D` |
| 服务生命周期 | 异步初始化、按子系统报告就绪/失败、排队/打断/取消、destroy 回收线程与服务连接、销毁后不回调 | `[destroy while speaking *]`、`[destroy in callback]` |
| MICROPHONE 权限 | 运行时探测音频服务与默认输入源；拒绝/无设备时以错误事件结束；应用内首次使用征得同意；Flatpak 声明 `--socket=pulseaudio` | `[microphone permission/device]`；`packaging/flatpak/` |
| 离线数据包随应用分发 | 语音模型/音色安装到 `share/p1-navigation/voice`，适配器按 `<exe>/../share/...` 查找；`tools/fetch-voice-deps.sh` 按 SHA256 复现 | `cmake --install` 检查（见第 7 节） |

导航引导与语音控制经 `VoiceNavigationBridge` 只连接 4 号 `NavigationController` 公共契约。4 号状态机尚未实现，因此本次用只实现契约的 `FakeController` 验证（`voice-bridge` 测试）；**接入真实状态机属于 4 号交付后的集成项**。

## 2. 结构

```text
导航核心 / Qt 应用                     libp1_voice.so（C ABI）
┌─────────────────────────────┐        ┌──────────────────────────────────────────┐
│ VoiceNavigationBridge        │        │ p1_voice_create/speak/listen/cancel/...   │
│  引导→播报文本、指令→契约命令 │        │  ┌ tts 线程：PiperTts（常驻 piper 子进程）  │
│ VoiceService（QObject）       │──C ABI─▶│  ├ asr 线程：VoskAsr（普通话模型）        │
│  回调→QueuedConnection→主线程 │◀─回调──│  └ dispatch 线程：唯一调用回调的线程      │
│ VoicePage（路由 "voice"）     │        │ AudioIo：PulseAudio 播放/采集、WAV 读写   │
└─────────────────────────────┘        └──────────────────────────────────────────┘
```

| 路径 | 作用 |
| --- | --- |
| `include/contracts/VoiceFfi.h` | FFI 契约：错误码、事件、配置、10 个函数、线程/所有权/生命周期规则 |
| `src/pal/linux/voice/LinuxVoiceAdapter.cpp` | C ABI 实现：三线程、请求队列、打断/取消、初始化与销毁顺序 |
| `src/pal/linux/voice/AudioIo.*` | libpulse-simple 播放/录音、麦克风探测（libpulse introspect）、WAV、16 kHz 重采样 |
| `src/pal/linux/voice/backends/PiperTts.cpp`、`VoskAsr.cpp` | 选定的两个引擎（试验期的 espeak-ng、speech-dispatcher、whisper.cpp 已删除） |
| `src/pal/linux/voice/tools/VoiceBench.cpp` | 经 C ABI 的基准程序 |
| `src/app/voice/VoiceService.*` | Qt 封装：按需启动、代际号丢弃旧实例残留信号 |
| `src/app/voice/VoiceNavigationBridge.*` | 引导播报去重、紧急打断、指令解析与分派 |
| `src/app/voice/VoicePage.*` | 语音助手页面 |

## 3. FFI 契约要点

- **线程**：所有函数线程安全、不阻塞在合成/识别上（`p1_voice_destroy` 除外）。回调只在适配器内部分发线程上串行调用，绝不在 `p1_voice_*` 调用栈内同步回调，所以调用方可以持锁调用 API。
- **回调内可做的事**：可以再次 speak/listen/cancel；不能 destroy（返回 `P1_VOICE_ERR_IN_CALLBACK`，不做任何事）。
- **所有权**：配置字符串与 speak 文本在调用期间复制；事件结构与 `text` 由适配器拥有，仅在回调期间有效。
- **初始化**：`create` 立即返回，TTS、ASR 在各自线程加载；每个子系统各发一次 `SERVICE_READY` 或 `SERVICE_ERROR`（`text` 以 `tts=`/`asr=` 开头）。一侧失败不影响另一侧（如识别模型缺失时仍可播报）。初始化前提交的 speak/listen 会排队。
- **打断与取消**：`P1_VOICE_SPEAK_INTERRUPT` 取消当前播报并清空队列（偏航、到达等紧急提示使用）；`p1_voice_cancel(id)` 取消指定请求，`id=0` 取消全部。
- **销毁顺序**：标记关闭 → 取消排队/在途请求（排队的立即产生 `*_CANCELLED`）→ 停止并 join tts/asr 线程（关闭 TTS 服务连接/子进程、释放模型）→ 送完已排队事件 → join 分发线程 → 释放。返回后保证无回调，`user_data` 可释放。
- **子进程所有权**：每个适配器拥有自己的 Piper 子进程（`posix_spawn`，stdin 写一行文本、stdout 读回 WAV 路径，模型只加载一次）。tts 线程屏蔽 SIGPIPE，子进程意外退出时播报以错误事件结束而不终止应用；销毁时关闭 stdin、等待退出（2 s 后 SIGKILL）并删除临时目录。

Qt 侧 `VoiceService` 在分发线程上只复制数据，再用 `QMetaObject::invokeMethod(self, …, Qt::QueuedConnection)` 交给主线程；每次 start/stop 增加代际号，旧实例迟到的信号被丢弃。

## 4. 识别流程

一次 `listen()` 打开输入（麦克风 16 kHz 单声道，或测试 WAV 自动重采样）→ 每 100 ms 送入 Vosk → 有新的中间结果时发 `LISTEN_PARTIAL` → Kaldi 端点检测判定句末（或 `stop_listening()`、超时、WAV 结束）→ `LISTEN_RESULT`（去掉中文词间空格）。没有文本时以 `NO_SPEECH`/`TIMEOUT` 错误结束。`stop_listening()` 用于按住说话松开，`timeout_ms` 默认 10 s。

## 5. 导航桥接

| 输入 | 行为 |
| --- | --- |
| `guidanceUpdated` | 同一 `index` 只播一次；暂停时不播；文本为空时按动作类型和距离生成（如“前方300米右转”，距离按 10/50 米取整，1 km 以上用公里） |
| `OffRouteDetected` / `Arrived` / `NavigationAborted` / `NavigationPaused` | 打断式播报 |
| `RouteUpdated` | 播“已为您重新规划路线”，并重置引导序号（新路线序号重新开始） |
| `NavigationStopped` | 取消全部播报 |
| 识别“暂停/继续/停止导航” | 直接调用契约 `pause()`/`resume()`/`stop()` |
| 识别“开始导航/重新规划/导航到某地/放大缩小” | 发信号交给应用层，桥接不越权构造规划请求 |
| 识别“还有多远” | 读 `snapshot().remainingDistanceM/DurationSec` 回答 |

指令解析容忍识别结果中的分词空格、标点、繁体字，以及“请/帮我/我想”等口语前缀；顺序从具体到一般（“关闭语音播报”不会被当成“停止导航”）。

## 6. 麦克风/定位权限与分发声明

任务书中的 LOCATION/MICROPHONE 不是 Linux 通用配置键，按分发方式分别落实：

| 分发方式 | MICROPHONE | LOCATION（4 号主责，1 号汇总） | 状态 |
| --- | --- | --- | --- |
| 原生安装（CMake install / 压缩包） | 无系统级逐应用授权；应用内首次“开始说话”时征得同意（本次会话有效），运行时 `p1_voice_check_microphone()` 探测音频服务与默认输入源（监视源判为“无真实麦克风”） | GeoClue2 按 desktop id `p1-navigation` 由桌面代理授权；已安装 `share/applications/p1-navigation.desktop` | 已实现并测试 |
| Flatpak | `--socket=pulseaudio`；未授予时连接失败 → `PERMISSION_DENIED`/`DEVICE` 错误事件 | `--system-talk-name=org.freedesktop.GeoClue2` | 清单模板 `packaging/flatpak/cn.p1.Navigation.yml`，**未验证**（环境无 flatpak-builder） |

拒绝/不可用的行为已测：音频服务不可达（`PULSE_SERVER` 指向不存在的 socket，模拟沙箱未授予）时 `listen()` 以 `DEVICE`/`PERMISSION_DENIED` 错误事件结束，不崩溃、不挂起。

## 7. 依赖、构建与离线资源

`tools/fetch-voice-deps.sh` 下载并按 SHA256 校验 Vosk 0.3.45、vosk-model-small-cn-0.22、Piper 2023.11.14-2 与 zh_CN-huayan-medium。内容放在 `third_party/voice/`（已 gitignore，不进 Git）。系统包只需 `libpulse-dev`（以及解压用的 `unzip`）。

缺少 Vosk 库时 CMake 配置报错；缺少离线资源时给出警告（运行时报告 `MODEL_MISSING`）。`cmake --install` 安装 `bin/p1-navigation`、`lib/libp1_voice.so*`、`lib/libvosk.so`、`share/p1-navigation/voice/`（离线资源）、`share/applications/p1-navigation.desktop` 与 `include/p1-navigation/VoiceFfi.h`；可执行程序 RPATH 为 `$ORIGIN/../lib`。资源查找顺序：配置 `resource_dir` → 环境变量 `P1_VOICE_RESOURCE_DIR` → `<exe>/../share/p1-navigation/voice` → 源码树 `third_party/voice/resources`（开发构建）。

许可：Vosk 与模型 Apache-2.0；Piper MIT（其包内的 espeak-ng 为 GPL-3.0、onnxruntime MIT，作为独立进程使用，不链接进本组程序）；zh_CN-huayan 音色许可未核实，正式发布前须查阅 rhasspy/piper-voices 的 MODEL_CARD。安装体积约 204 MiB（Piper 52 + 音色 61 + Vosk 模型 66 + libvosk 25）。

## 8. 测试

| CTest | 内容 |
| --- | --- |
| `voice-ffi-c-smoke` | C99 程序经 FFI 创建→Piper 播报→等待回调→销毁 |
| `voice-adapter` | 101 项检查：播报/初始化前排队/打断/排队取消/参数；识别/静音/忙/取消；识别模型缺失时降级（播报仍可用）；回调内销毁被拒；播报中销毁（约 20 ms，销毁后零回调）；两实例并存；未知后端；音频服务不可达；在识别结果回调中立即再次 listen（3 次，回归测试） |
| `voice-bridge` | 22 条指令解析、播报文本格式、引导去重/暂停/偏航打断/重算重置/关闭播报、“暂停/停止导航”语音经 Vosk 识别后驱动控制器、剩余距离回答、服务 stop/restart、默认后端 |

**限制**：统一虚拟机（Hyper-V）没有声卡，PipeWire 只有 `auto_null` 输出与其监视源。播放实际送到空设备（按实时推进），但**真实扬声器听感与真实麦克风识别未验证**；ASR 质量用 TTS 合成的语音测得，不能代表真人语音，需在有麦克风的机器上补测。

## 9. 方案对比结果

三种候选组合（A espeak-ng+Vosk、B speech-dispatcher+Vosk、C Piper+whisper.cpp）在同一 C ABI、同一 20 句、同一虚拟机下实测：espeak-ng 普通话音色的可懂度代理很差（用其合成语音做识别 CER > 100%），Piper 为 8–11%；Vosk 的 CER 8.0%、句末延迟 58 ms，whisper.cpp base 为 10.7%、859 ms。组员 1 选定由两者组成的 **Piper + Vosk**，未选后端已删除。详见 `decision_log.md` 的 `DEC-20261123-1-voice-backend-01` 与 `docs/benchmarks/navit/NAVIT-20261123-1-voice-01.md`。
