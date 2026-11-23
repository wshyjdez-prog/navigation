#include "pal/linux/voice/AudioIo.h"

#include "contracts/VoiceFfi.h"

#include <pulse/error.h>
#include <pulse/mainloop.h>
#include <pulse/context.h>
#include <pulse/introspect.h>
#include <pulse/simple.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

namespace p1::voice {
namespace {

int mapPulseError(int paError) {
    switch (paError) {
    case PA_ERR_ACCESS:
    case PA_ERR_AUTHKEY:
        return P1_VOICE_ERR_PERMISSION_DENIED;
    case PA_ERR_CONNECTIONREFUSED:
    case PA_ERR_NOENTITY:
    case PA_ERR_INVALIDSERVER:
    case PA_ERR_CONNECTIONTERMINATED:
        return P1_VOICE_ERR_DEVICE;
    default:
        return P1_VOICE_ERR_DEVICE;
    }
}

class PulseSink final : public AudioSink {
public:
    explicit PulseSink(pa_simple* s) : s_(s) {}
    ~PulseSink() override { if (s_) pa_simple_free(s_); }
    bool write(const int16_t* pcm, std::size_t samples, AudioError& error) override {
        int e = 0;
        if (pa_simple_write(s_, pcm, samples * sizeof(int16_t), &e) < 0) {
            error = {mapPulseError(e), std::string("pa_simple_write: ") + pa_strerror(e)};
            return false;
        }
        return true;
    }
    bool drain(AudioError& error) override {
        int e = 0;
        if (pa_simple_drain(s_, &e) < 0) {
            error = {mapPulseError(e), std::string("pa_simple_drain: ") + pa_strerror(e)};
            return false;
        }
        return true;
    }
    void abort() override { int e = 0; pa_simple_flush(s_, &e); }
private:
    pa_simple* s_;
};

class WavSink final : public AudioSink {
public:
    WavSink(std::string path, int rate) : path_(std::move(path)), rate_(rate) {}
    bool write(const int16_t* pcm, std::size_t samples, AudioError&) override {
        data_.insert(data_.end(), pcm, pcm + samples);
        return true;
    }
    bool drain(AudioError& error) override {
        std::string msg;
        if (!writeWav(path_, rate_, data_, msg)) { error = {P1_VOICE_ERR_DEVICE, msg}; return false; }
        return true;
    }
    void abort() override {}
private:
    std::string path_;
    int rate_;
    std::vector<int16_t> data_;
};

class PulseSource final : public AudioSource {
public:
    explicit PulseSource(pa_simple* s) : s_(s) {}
    ~PulseSource() override { if (s_) pa_simple_free(s_); }
    long read(int16_t* pcm, std::size_t samples, AudioError& error) override {
        int e = 0;
        if (pa_simple_read(s_, pcm, samples * sizeof(int16_t), &e) < 0) {
            error = {mapPulseError(e), std::string("pa_simple_read: ") + pa_strerror(e)};
            return -1;
        }
        return static_cast<long>(samples);
    }
private:
    pa_simple* s_;
};

class WavSource final : public AudioSource {
public:
    WavSource(std::vector<int16_t> data, bool realtime, int rate)
        : data_(std::move(data)), realtime_(realtime), rate_(rate),
          start_(std::chrono::steady_clock::now()) {}
    long read(int16_t* pcm, std::size_t samples, AudioError&) override {
        if (pos_ >= data_.size()) return 0;
        const std::size_t n = std::min(samples, data_.size() - pos_);
        std::memcpy(pcm, data_.data() + pos_, n * sizeof(int16_t));
        pos_ += n;
        if (realtime_) {
            const auto due = start_ + std::chrono::microseconds(
                static_cast<long long>(pos_) * 1000000LL / rate_);
            std::this_thread::sleep_until(due);
        }
        return static_cast<long>(n);
    }
private:
    std::vector<int16_t> data_;
    bool realtime_;
    int rate_;
    std::size_t pos_ = 0;
    std::chrono::steady_clock::time_point start_;
};

std::uint32_t le32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24); }
std::uint16_t le16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
void put32(std::string& s, std::uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(char((v >> (8 * i)) & 0xff)); }
void put16(std::string& s, std::uint16_t v) { s.push_back(char(v & 0xff)); s.push_back(char(v >> 8)); }

}  // namespace

std::unique_ptr<AudioSink> openSink(const std::string& wavPath, int sampleRate, AudioError& error) {
    if (!wavPath.empty()) return std::make_unique<WavSink>(wavPath, sampleRate);
    pa_sample_spec ss{PA_SAMPLE_S16LE, static_cast<std::uint32_t>(sampleRate), 1};
    pa_buffer_attr attr;
    attr.maxlength = static_cast<std::uint32_t>(-1);
    attr.tlength = static_cast<std::uint32_t>(sampleRate * 2 / 10);  // 100 ms 目标缓冲，利于快速打断
    attr.prebuf = static_cast<std::uint32_t>(-1);
    attr.minreq = static_cast<std::uint32_t>(-1);
    attr.fragsize = static_cast<std::uint32_t>(-1);
    int e = 0;
    pa_simple* s = pa_simple_new(nullptr, "P1 Navigation", PA_STREAM_PLAYBACK, nullptr,
                                 "voice-guidance", &ss, nullptr, &attr, &e);
    if (!s) {
        error = {mapPulseError(e), std::string("无法打开播放流: ") + pa_strerror(e)};
        return nullptr;
    }
    return std::make_unique<PulseSink>(s);
}

std::unique_ptr<AudioSource> openSource(const std::string& wavPath, bool realtime, int sampleRate,
                                        AudioError& error) {
    if (!wavPath.empty()) {
        WavData wav;
        std::string msg;
        if (!readWav(wavPath, wav, msg)) { error = {P1_VOICE_ERR_DEVICE, msg}; return nullptr; }
        return std::make_unique<WavSource>(toMono16k(wav, sampleRate), realtime, sampleRate);
    }
    pa_sample_spec ss{PA_SAMPLE_S16LE, static_cast<std::uint32_t>(sampleRate), 1};
    pa_buffer_attr attr;
    attr.maxlength = static_cast<std::uint32_t>(-1);
    attr.fragsize = static_cast<std::uint32_t>(sampleRate * 2 / 10);  // 100 ms
    attr.tlength = attr.prebuf = attr.minreq = static_cast<std::uint32_t>(-1);
    int e = 0;
    pa_simple* s = pa_simple_new(nullptr, "P1 Navigation", PA_STREAM_RECORD, nullptr,
                                 "voice-command", &ss, nullptr, &attr, &e);
    if (!s) {
        error = {mapPulseError(e), std::string("无法打开麦克风: ") + pa_strerror(e)};
        return nullptr;
    }
    return std::make_unique<PulseSource>(s);
}

int probeMicrophone(std::string& sourceName) {
    struct Probe {
        pa_mainloop* loop = nullptr;
        int result = P1_VOICE_ERR_DEVICE;
        bool done = false;
        std::string defaultSource;
        std::string name;
    } probe;
    probe.loop = pa_mainloop_new();
    if (!probe.loop) return P1_VOICE_ERR_INTERNAL;
    pa_context* ctx = pa_context_new(pa_mainloop_get_api(probe.loop), "P1 Navigation microphone probe");
    if (!ctx || pa_context_connect(ctx, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
        const int err = ctx ? pa_context_errno(ctx) : PA_ERR_INTERNAL;
        if (ctx) pa_context_unref(ctx);
        pa_mainloop_free(probe.loop);
        sourceName = std::string("音频服务不可连接: ") + pa_strerror(err);
        return mapPulseError(err);
    }
    pa_context_set_state_callback(ctx, [](pa_context* c, void* u) {
        auto* p = static_cast<Probe*>(u);
        switch (pa_context_get_state(c)) {
        case PA_CONTEXT_READY:
            pa_operation_unref(pa_context_get_server_info(c, [](pa_context* c2, const pa_server_info* i, void* u2) {
                auto* p2 = static_cast<Probe*>(u2);
                p2->defaultSource = (i && i->default_source_name) ? i->default_source_name : "";
                if (p2->defaultSource.empty()) { p2->result = P1_VOICE_ERR_DEVICE; p2->done = true; return; }
                pa_operation_unref(pa_context_get_source_info_by_name(c2, p2->defaultSource.c_str(),
                    [](pa_context*, const pa_source_info* si, int eol, void* u3) {
                        auto* p3 = static_cast<Probe*>(u3);
                        if (eol) { p3->done = true; return; }
                        if (!si) return;
                        p3->name = si->description ? si->description : si->name;
                        // monitor 源只是播放回环，不是真实麦克风。
                        p3->result = (si->monitor_of_sink == PA_INVALID_INDEX) ? P1_VOICE_OK : P1_VOICE_ERR_DEVICE;
                    }, p2));
            }, p));
            break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            p->result = mapPulseError(pa_context_errno(c));
            p->done = true;
            break;
        default:
            break;
        }
    }, &probe);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!probe.done && std::chrono::steady_clock::now() < deadline) {
        pa_mainloop_iterate(probe.loop, 0, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!probe.done) probe.result = P1_VOICE_ERR_TIMEOUT;
    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(probe.loop);
    sourceName = probe.name.empty() ? probe.defaultSource : probe.name;
    if (probe.result == P1_VOICE_ERR_DEVICE && !probe.defaultSource.empty())
        sourceName += "（监视源，非真实麦克风）";
    return probe.result;
}

bool readWav(const std::string& path, WavData& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "无法打开 WAV: " + path; return false; }
    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (buf.size() < 12 || std::memcmp(buf.data(), "RIFF", 4) || std::memcmp(buf.data() + 8, "WAVE", 4)) {
        error = "不是 RIFF/WAVE 文件: " + path;
        return false;
    }
    std::size_t p = 12;
    int bits = 0, format = 0;
    bool fmtSeen = false;
    while (p + 8 <= buf.size()) {
        const std::uint32_t size = le32(&buf[p + 4]);
        const unsigned char* body = &buf[p + 8];
        if (p + 8 + size > buf.size()) break;
        if (!std::memcmp(&buf[p], "fmt ", 4) && size >= 16) {
            format = le16(body);
            out.channels = le16(body + 2);
            out.sampleRate = static_cast<int>(le32(body + 4));
            bits = le16(body + 14);
            fmtSeen = true;
        } else if (!std::memcmp(&buf[p], "data", 4)) {
            if (!fmtSeen || format != 1 || bits != 16 || out.channels < 1) {
                error = "仅支持 16 位 PCM WAV: " + path;
                return false;
            }
            out.samples.resize(size / 2);
            std::memcpy(out.samples.data(), body, out.samples.size() * 2);
            return true;
        }
        p += 8 + size + (size & 1);
    }
    error = "WAV 缺少 data 块: " + path;
    return false;
}

bool writeWav(const std::string& path, int rate, const std::vector<int16_t>& mono, std::string& error) {
    std::string h = "RIFF";
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(mono.size() * 2);
    put32(h, 36 + dataBytes);
    h += "WAVEfmt ";
    put32(h, 16); put16(h, 1); put16(h, 1);
    put32(h, static_cast<std::uint32_t>(rate)); put32(h, static_cast<std::uint32_t>(rate * 2));
    put16(h, 2); put16(h, 16);
    h += "data";
    put32(h, dataBytes);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { error = "无法写入 WAV: " + path; return false; }
    out.write(h.data(), static_cast<std::streamsize>(h.size()));
    out.write(reinterpret_cast<const char*>(mono.data()), dataBytes);
    return static_cast<bool>(out);
}

std::vector<int16_t> toMono16k(const WavData& wav, int target) {
    std::vector<float> mono(wav.samples.size() / static_cast<std::size_t>(wav.channels));
    for (std::size_t i = 0; i < mono.size(); ++i) {
        int sum = 0;
        for (int c = 0; c < wav.channels; ++c) sum += wav.samples[i * wav.channels + c];
        mono[i] = static_cast<float>(sum) / wav.channels;
    }
    if (wav.sampleRate == target) {
        std::vector<int16_t> out(mono.size());
        for (std::size_t i = 0; i < mono.size(); ++i) out[i] = static_cast<int16_t>(mono[i]);
        return out;
    }
    // 线性插值重采样；对语音识别（16 kHz）足够，避免额外依赖。
    const double ratio = static_cast<double>(wav.sampleRate) / target;
    const std::size_t n = static_cast<std::size_t>(mono.size() / ratio);
    std::vector<int16_t> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double src = i * ratio;
        const std::size_t a = static_cast<std::size_t>(src);
        const std::size_t b = std::min(a + 1, mono.size() - 1);
        const double t = src - a;
        out[i] = static_cast<int16_t>(mono[a] * (1 - t) + mono[b] * t);
    }
    return out;
}

}  // namespace p1::voice
