#pragma once
// LinuxVoiceAdapter 内部的 TTS/ASR 引擎接口（不跨 FFI 边界）。
// 每个引擎实例只在一个适配器工作线程上使用；init/speak/feed 等都在该线程调用，
// 只有 interrupt() 可从其他线程调用，用于尽快打断阻塞中的播报。
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace p1::voice {

struct VoiceSettings {
    std::string ttsBackend;
    std::string asrBackend;
    std::string language = "zh-CN";
    std::string resourceDir;      // 已解析的资源根目录
    std::string outputWavPath;    // 非空：TTS 写 WAV 不播放
    std::string inputWavPath;     // 非空：ASR 读 WAV 不采集
    bool inputRealtime = false;
    int speechRate = 0;           // -100..100
};

// 引擎返回值沿用 P1VoiceError 数值，message 给出人类可读原因。
struct EngineStatus {
    int code = 0;
    std::string message;
    bool ok() const { return code == 0; }
};

class TtsEngine {
public:
    virtual ~TtsEngine() = default;
    virtual const char* name() const = 0;
    virtual EngineStatus init(const VoiceSettings& settings) = 0;
    // 阻塞直到播放完毕或被取消。首个音频块交给输出时调用 onAudioStart（至多一次）。
    // 取消时返回 code = 1（非错误，表示已打断）。
    virtual EngineStatus speak(const std::string& utf8, const std::atomic<bool>& cancelled,
                               const std::function<void()>& onAudioStart) = 0;
    // 任意线程：尽快结束正在进行的 speak()。
    virtual void interrupt() {}
    // 引擎相关的子进程（如外部合成进程），供基准统计内存；无则返回空。
    virtual std::vector<int> helperPids() const { return {}; }
};

class AsrEngine {
public:
    virtual ~AsrEngine() = default;
    virtual const char* name() const = 0;
    virtual EngineStatus init(const VoiceSettings& settings) = 0;
    virtual int sampleRate() const { return 16000; }
    virtual EngineStatus begin() = 0;
    // 送入 16 kHz 单声道 int16；partial 非空表示有新的中间结果；endpoint 表示句末。
    virtual EngineStatus feed(const int16_t* pcm, std::size_t samples, std::string& partial,
                              bool& endpoint) = 0;
    // 结束本句并取得最终文本。
    virtual EngineStatus finish(std::string& text, float& confidence) = 0;
};

std::unique_ptr<TtsEngine> createTtsEngine(const std::string& backend);
std::unique_ptr<AsrEngine> createAsrEngine(const std::string& backend);
const char* compiledBackends();
const char* defaultTtsBackend();
const char* defaultAsrBackend();

}  // namespace p1::voice
