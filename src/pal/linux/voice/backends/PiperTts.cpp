// TTS：Piper（VITS 神经网络，onnxruntime）中文音色 zh_CN-huayan-medium。
// Piper 以常驻子进程运行（--output_dir 模式）：每写入一行文本，它合成一个 WAV 并在
// stdout 输出路径；适配器读取后经 PulseAudio 播放。模型只加载一次。
// 子进程由本引擎拥有：析构时关闭 stdin 并回收（超时则 SIGKILL）。
#include "pal/linux/voice/AudioIo.h"
#include "pal/linux/voice/VoiceEngine.h"

#include "contracts/VoiceFfi.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace p1::voice {
namespace {

namespace fs = std::filesystem;

class PiperTts final : public TtsEngine {
public:
    ~PiperTts() override { shutdown(); }
    const char* name() const override { return "piper"; }

    EngineStatus init(const VoiceSettings& s) override {
        settings_ = s;
        const fs::path root = fs::path(s.resourceDir);
        const fs::path exe = root / "piper" / "piper";
        const fs::path model = root / "piper-voice" / "zh_CN-huayan-medium.onnx";
        if (!fs::exists(exe)) return {P1_VOICE_ERR_BACKEND_UNAVAILABLE, "缺少 Piper 程序: " + exe.string()};
        if (!fs::exists(model) || !fs::exists(model.string() + ".json"))
            return {P1_VOICE_ERR_MODEL_MISSING, "缺少 Piper 中文音色: " + model.string()};
        char tmpl[] = "/tmp/p1-piper-XXXXXX";
        if (!mkdtemp(tmpl)) return {P1_VOICE_ERR_INTERNAL, "无法创建 Piper 临时目录"};
        outDir_ = tmpl;

        int in[2], out[2];
        if (pipe2(in, O_CLOEXEC) != 0 || pipe2(out, O_CLOEXEC) != 0)
            return {P1_VOICE_ERR_INTERNAL, "pipe 失败"};
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, in[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&fa, out[1], STDOUT_FILENO);
        posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        const double lengthScale = 1.0 - s.speechRate / 200.0;
        const std::string ls = std::to_string(lengthScale);
        const std::string espeakData = (root / "piper" / "espeak-ng-data").string();
        std::vector<std::string> args = {exe.string(), "-q", "-m", model.string(), "-d", outDir_,
                                         "--length_scale", ls, "--sentence_silence", "0.1",
                                         "--espeak_data", espeakData};
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        const int rc = posix_spawn(&pid_, exe.c_str(), &fa, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        ::close(in[0]);
        ::close(out[1]);
        if (rc != 0) {
            ::close(in[1]);
            ::close(out[0]);
            pid_ = -1;
            return {P1_VOICE_ERR_BACKEND_UNAVAILABLE, std::string("无法启动 Piper: ") + std::strerror(rc)};
        }
        toPiper_ = in[1];
        fromPiper_ = out[0];
        // 预热：首句合成会初始化 onnxruntime 会话，放在初始化阶段而不是首次播报。
        std::string wav, err;
        if (!synthesize("导航", wav, err, nullptr)) return {P1_VOICE_ERR_BACKEND_UNAVAILABLE, err};
        std::error_code ec;
        fs::remove(wav, ec);
        return {};
    }

    EngineStatus speak(const std::string& text, const std::atomic<bool>& cancelled,
                       const std::function<void()>& onStart) override {
        std::string wavPath, err;
        if (!synthesize(text, wavPath, err, &cancelled)) {
            if (cancelled.load()) return {1, "cancelled"};
            return {P1_VOICE_ERR_BACKEND_UNAVAILABLE, err};
        }
        WavData wav;
        const bool okRead = readWav(wavPath, wav, err);
        std::error_code ec;
        fs::remove(wavPath, ec);
        if (!okRead) return {P1_VOICE_ERR_INTERNAL, err};
        if (cancelled.load()) return {1, "cancelled"};
        AudioError aerr;
        auto sink = openSink(settings_.outputWavPath, wav.sampleRate, aerr);
        if (!sink) return {aerr.code, aerr.message};
        const std::size_t chunk = static_cast<std::size_t>(wav.sampleRate / 50);  // 20 ms
        bool started = false;
        for (std::size_t off = 0; off < wav.samples.size(); off += chunk) {
            if (cancelled.load()) { sink->abort(); return {1, "cancelled"}; }
            const std::size_t n = std::min(chunk, wav.samples.size() - off);
            if (!sink->write(wav.samples.data() + off, n, aerr)) return {aerr.code, aerr.message};
            if (!started) { started = true; onStart(); }
        }
        if (!sink->drain(aerr)) return {aerr.code, aerr.message};
        return cancelled.load() ? EngineStatus{1, "cancelled"} : EngineStatus{};
    }

    std::vector<int> helperPids() const override {
        return pid_ > 0 ? std::vector<int>{pid_} : std::vector<int>{};
    }

private:
    bool synthesize(std::string text, std::string& wavPath, std::string& err, const std::atomic<bool>* cancelled) {
        if (pid_ <= 0) { err = "Piper 进程未运行"; return false; }
        for (char& c : text) if (c == '\n' || c == '\r') c = ' ';
        text.push_back('\n');
        if (::write(toPiper_, text.data(), text.size()) != static_cast<ssize_t>(text.size())) {
            err = "写入 Piper 失败（进程已退出？）";
            return false;
        }
        // 读取一行输出路径；合成不可中途打断，取消时仍读完本句再丢弃，保持协议同步。
        std::string line;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (true) {
            const auto nl = pending_.find('\n');
            if (nl != std::string::npos) {
                line = pending_.substr(0, nl);
                pending_.erase(0, nl + 1);
                break;
            }
            pollfd pfd{fromPiper_, POLLIN, 0};
            const int pr = ::poll(&pfd, 1, 50);
            if (pr > 0) {
                char buf[1024];
                const ssize_t n = ::read(fromPiper_, buf, sizeof buf);
                if (n <= 0) { err = "Piper 进程意外退出"; return false; }
                pending_.append(buf, static_cast<std::size_t>(n));
            }
            if (std::chrono::steady_clock::now() > deadline) { err = "Piper 合成超时"; return false; }
        }
        wavPath = line;
        if (cancelled && cancelled->load()) {
            std::error_code ec;
            fs::remove(wavPath, ec);
            return false;
        }
        return true;
    }

    void shutdown() {
        if (toPiper_ >= 0) { ::close(toPiper_); toPiper_ = -1; }
        if (pid_ > 0) {
            int status = 0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (waitpid(pid_, &status, WNOHANG) == 0) {
                if (std::chrono::steady_clock::now() > deadline) {
                    ::kill(pid_, SIGKILL);
                    waitpid(pid_, &status, 0);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            pid_ = -1;
        }
        if (fromPiper_ >= 0) { ::close(fromPiper_); fromPiper_ = -1; }
        if (!outDir_.empty()) { std::error_code ec; fs::remove_all(outDir_, ec); }
    }

    VoiceSettings settings_;
    pid_t pid_ = -1;
    int toPiper_ = -1;
    int fromPiper_ = -1;
    std::string pending_;
    std::string outDir_;
};

}  // namespace

std::unique_ptr<TtsEngine> makePiperTts() { return std::make_unique<PiperTts>(); }

}  // namespace p1::voice
