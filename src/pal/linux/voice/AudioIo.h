#pragma once
// PulseAudio（PipeWire 兼容层）播放/采集与 WAV 读写。
// Linux 桌面上的麦克风授权表现为能否连接音频服务并打开录音流：
// Flatpak 未授予 --socket=pulseaudio、Snap 未连接 audio-record 时连接/打开会失败。
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct pa_simple;

namespace p1::voice {

struct AudioError {
    int code = 0;  // P1VoiceError
    std::string message;
};

// 输出：播放到默认音频设备，或写入 WAV（测试/基准）。
class AudioSink {
public:
    virtual ~AudioSink() = default;
    virtual bool write(const int16_t* pcm, std::size_t samples, AudioError& error) = 0;
    virtual bool drain(AudioError& error) = 0;
    virtual void abort() = 0;  // 丢弃未播放的数据
};

std::unique_ptr<AudioSink> openSink(const std::string& wavPathOrEmpty, int sampleRate, AudioError& error);

// 输入：采集默认麦克风，或读取 WAV（自动转为 16 kHz 单声道）。
class AudioSource {
public:
    virtual ~AudioSource() = default;
    // 读取最多 samples 个样本；返回实际个数，0 表示输入结束，-1 表示失败。
    virtual long read(int16_t* pcm, std::size_t samples, AudioError& error) = 0;
};

std::unique_ptr<AudioSource> openSource(const std::string& wavPathOrEmpty, bool realtime,
                                        int sampleRate, AudioError& error);

// 检查音频服务与默认输入源，返回 P1VoiceError 数值。
int probeMicrophone(std::string& sourceName);

struct WavData {
    int sampleRate = 0;
    int channels = 0;
    std::vector<int16_t> samples;  // 交错
};
bool readWav(const std::string& path, WavData& out, std::string& error);
bool writeWav(const std::string& path, int sampleRate, const std::vector<int16_t>& mono, std::string& error);
std::vector<int16_t> toMono16k(const WavData& wav, int targetRate);

}  // namespace p1::voice
