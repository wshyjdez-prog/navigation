// LinuxVoiceAdapter：VoiceFfi.h 的实现。
//
// 线程模型（每个适配器实例）：
//   tts 线程      —— 创建/初始化 TTS 引擎（Piper 常驻子进程），按队列逐条播报；
//   asr 线程      —— 创建/初始化 ASR 引擎（Vosk 模型），执行一次一个的 listen 会话；
//   dispatch 线程 —— 唯一调用核心层回调的线程，按发生顺序串行送达事件。
// 工作线程只把事件压入分发队列，不直接回调，因此核心层回调变慢不会卡住音频，
// 核心层也可以在回调里再次调用 speak/listen/cancel。
#include "contracts/VoiceFfi.h"

#include "pal/linux/voice/AudioIo.h"
#include "pal/linux/voice/VoiceEngine.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <pthread.h>
#include <time.h>
#include <unistd.h>

using namespace p1::voice;

namespace {

std::int64_t unixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t monotonicUs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

std::string resolveResourceDir(const char* configured) {
    namespace fs = std::filesystem;
    if (configured && *configured) return configured;
    if (const char* env = std::getenv("P1_VOICE_RESOURCE_DIR"); env && *env) return env;
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        const fs::path installed = exe.parent_path().parent_path() / "share" / "p1-navigation" / "voice";
        if (fs::exists(installed)) return installed.string();
    }
#ifdef P1_VOICE_DEV_RESOURCE_DIR
    return P1_VOICE_DEV_RESOURCE_DIR;
#else
    return {};
#endif
}

// 工作线程屏蔽 SIGPIPE：Piper 子进程退出时写管道返回 EPIPE，而不是终止整个应用。
void blockSigpipe() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

struct PendingEvent {
    int type = 0;
    std::uint64_t requestId = 0;
    int error = 0;
    float confidence = -1.0f;
    std::string text;
    std::int64_t unixMs = 0;
    std::int64_t monoUs = 0;
};

struct SpeakRequest {
    std::uint64_t id = 0;
    std::string text;
};

struct ListenRequest {
    std::uint64_t id = 0;
    std::uint32_t timeoutMs = 0;
};

constexpr int kInitPending = 0;
constexpr int kInitOk = 1;
constexpr int kInitFailed = 2;

}  // namespace

struct P1VoiceAdapter {
    VoiceSettings settings;
    P1VoiceCallback callback = nullptr;
    void* userData = nullptr;
    std::atomic<std::uint64_t> nextId{1};
    std::atomic<bool> closing{false};

    // 分发
    std::mutex evMutex;
    std::condition_variable evCv;
    std::deque<PendingEvent> events;
    bool evStop = false;
    std::thread dispatcher;
    std::atomic<std::thread::id> dispatcherId{};

    // 初始化结果
    std::mutex initMutex;
    int ttsInit = kInitPending;
    int asrInit = kInitPending;
    EngineStatus ttsStatus, asrStatus;
    std::string ttsName, asrName;

    // TTS
    std::mutex ttsMutex;
    std::condition_variable ttsCv;
    std::deque<SpeakRequest> speakQueue;
    std::uint64_t currentSpeak = 0;
    std::atomic<bool> speakCancel{false};
    TtsEngine* ttsEngine = nullptr;  // 由 tts 线程拥有；interrupt() 在 ttsMutex 下跨线程调用
    bool ttsStop = false;
    std::thread ttsThread;

    // ASR
    std::mutex asrMutex;
    std::condition_variable asrCv;
    std::optional<ListenRequest> pendingListen;
    std::uint64_t currentListen = 0;
    std::atomic<bool> listenCancel{false};
    std::atomic<bool> listenStop{false};
    bool asrStop = false;
    std::thread asrThread;

    void post(int type, std::uint64_t id, int error = 0, std::string text = {}, float confidence = -1.0f) {
        PendingEvent e;
        e.type = type;
        e.requestId = id;
        e.error = error;
        e.text = std::move(text);
        e.confidence = confidence;
        e.unixMs = unixMs();
        e.monoUs = monotonicUs();
        {
            std::lock_guard<std::mutex> lock(evMutex);
            events.push_back(std::move(e));
        }
        evCv.notify_one();
    }

    // 请求终态：先在锁内清除“在途”标记再发事件，保证核心层在回调里立即发起
    // 下一次 listen/speak 时不会因旧请求尚未收尾而得到 BUSY。
    void endListen(int type, std::uint64_t id, int error = 0, std::string text = {}, float confidence = -1.0f) {
        std::lock_guard<std::mutex> lock(asrMutex);
        if (currentListen == id) currentListen = 0;
        post(type, id, error, std::move(text), confidence);
    }
    void endSpeak(int type, std::uint64_t id, int error = 0, std::string text = {}) {
        std::lock_guard<std::mutex> lock(ttsMutex);
        if (currentSpeak == id) currentSpeak = 0;
        post(type, id, error, std::move(text));
    }

    void dispatchLoop() {
        dispatcherId = std::this_thread::get_id();
        std::unique_lock<std::mutex> lock(evMutex);
        for (;;) {
            evCv.wait(lock, [&] { return evStop || !events.empty(); });
            if (events.empty()) {
                if (evStop) return;
                continue;
            }
            PendingEvent e = std::move(events.front());
            events.pop_front();
            lock.unlock();
            P1VoiceEvent ev{};
            ev.struct_size = sizeof(P1VoiceEvent);
            ev.type = e.type;
            ev.request_id = e.requestId;
            ev.error_code = e.error;
            ev.confidence = e.confidence;
            ev.text = e.text.c_str();
            ev.timestamp_ms = e.unixMs;
            ev.monotonic_us = e.monoUs;
            if (callback) callback(&ev, userData);
            lock.lock();
        }
    }

    // 每个子系统初始化完成即报告自己的状态：TTS 就绪不必等待较慢的识别模型加载，
    // 且 "tts=..." 的 SERVICE_READY 一定先于任何 SPEAK_* 事件送达（ASR 同理）。
    void reportInit(const char* subsystem, const std::string& backend, const EngineStatus& st) {
        if (st.ok()) post(P1_VOICE_EVENT_SERVICE_READY, 0, 0, std::string(subsystem) + "=" + backend);
        else post(P1_VOICE_EVENT_SERVICE_ERROR, 0, st.code, std::string(subsystem) + "=" + backend + ": " + st.message);
    }

    void ttsLoop() {
        blockSigpipe();
        std::unique_ptr<TtsEngine> engine = createTtsEngine(settings.ttsBackend);
        EngineStatus st = engine ? engine->init(settings)
                                 : EngineStatus{P1_VOICE_ERR_BACKEND_UNAVAILABLE, "未编译 TTS 后端: " + settings.ttsBackend};
        {
            std::lock_guard<std::mutex> lock(initMutex);
            ttsStatus = st;
            ttsName = settings.ttsBackend;
            ttsInit = st.ok() ? kInitOk : kInitFailed;
        }
        if (st.ok()) {
            std::lock_guard<std::mutex> lock(ttsMutex);
            ttsEngine = engine.get();
        }
        reportInit("tts", settings.ttsBackend, st);
        for (;;) {
            SpeakRequest req;
            {
                std::unique_lock<std::mutex> lock(ttsMutex);
                ttsCv.wait(lock, [&] { return ttsStop || !speakQueue.empty(); });
                if (speakQueue.empty()) break;  // ttsStop 且队列已清空
                req = std::move(speakQueue.front());
                speakQueue.pop_front();
                currentSpeak = req.id;
                speakCancel = false;
            }
            if (!st.ok()) {
                endSpeak(P1_VOICE_EVENT_ERROR, req.id, st.code, "tts: " + st.message);
            } else {
                const EngineStatus r = engine->speak(req.text, speakCancel, [&] {
                    post(P1_VOICE_EVENT_SPEAK_STARTED, req.id);
                });
                if (r.code == 0) endSpeak(P1_VOICE_EVENT_SPEAK_FINISHED, req.id);
                else if (r.code == 1) endSpeak(P1_VOICE_EVENT_SPEAK_CANCELLED, req.id);
                else endSpeak(P1_VOICE_EVENT_ERROR, req.id, r.code, "tts: " + r.message);
            }
        }
        {
            std::lock_guard<std::mutex> lock(ttsMutex);
            ttsEngine = nullptr;
        }
        engine.reset();  // 关闭 TTS 服务连接/子进程
    }

    void asrLoop() {
        blockSigpipe();
        std::unique_ptr<AsrEngine> engine = createAsrEngine(settings.asrBackend);
        EngineStatus st = engine ? engine->init(settings)
                                 : EngineStatus{P1_VOICE_ERR_BACKEND_UNAVAILABLE, "未编译 ASR 后端: " + settings.asrBackend};
        {
            std::lock_guard<std::mutex> lock(initMutex);
            asrStatus = st;
            asrName = settings.asrBackend;
            asrInit = st.ok() ? kInitOk : kInitFailed;
        }
        reportInit("asr", settings.asrBackend, st);
        for (;;) {
            ListenRequest req;
            {
                std::unique_lock<std::mutex> lock(asrMutex);
                asrCv.wait(lock, [&] { return asrStop || pendingListen.has_value(); });
                if (!pendingListen) break;
                req = *pendingListen;
                pendingListen.reset();
                currentListen = req.id;
                listenCancel = false;
                listenStop = false;
            }
            if (!st.ok()) endListen(P1_VOICE_EVENT_ERROR, req.id, st.code, "asr: " + st.message);
            else runListen(*engine, req);
        }
        engine.reset();
    }

    void runListen(AsrEngine& engine, const ListenRequest& req) {
        AudioError aerr;
        auto source = openSource(settings.inputWavPath, settings.inputRealtime, engine.sampleRate(), aerr);
        if (!source) { endListen(P1_VOICE_EVENT_ERROR, req.id, aerr.code, aerr.message); return; }
        engine.begin();
        post(P1_VOICE_EVENT_LISTEN_STARTED, req.id);
        const std::size_t chunk = static_cast<std::size_t>(engine.sampleRate() / 10);  // 100 ms
        std::vector<int16_t> buf(chunk);
        const auto start = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::milliseconds(req.timeoutMs ? req.timeoutMs : 10000);
        bool timedOut = false;
        for (;;) {
            if (listenCancel.load()) { endListen(P1_VOICE_EVENT_LISTEN_CANCELLED, req.id); return; }
            if (listenStop.load()) break;
            if (std::chrono::steady_clock::now() - start > timeout) { timedOut = true; break; }
            const long n = source->read(buf.data(), buf.size(), aerr);
            if (n < 0) { endListen(P1_VOICE_EVENT_ERROR, req.id, aerr.code, aerr.message); return; }
            if (n == 0) break;  // WAV 输入结束
            std::string partial;
            bool endpoint = false;
            const EngineStatus fr = engine.feed(buf.data(), static_cast<std::size_t>(n), partial, endpoint);
            if (!fr.ok()) { endListen(P1_VOICE_EVENT_ERROR, req.id, fr.code, "asr: " + fr.message); return; }
            if (!partial.empty()) post(P1_VOICE_EVENT_LISTEN_PARTIAL, req.id, 0, partial);
            if (endpoint) break;  // Vosk（Kaldi）端点检测判定句末
        }
        if (listenCancel.load()) { endListen(P1_VOICE_EVENT_LISTEN_CANCELLED, req.id); return; }
        std::string text;
        float confidence = -1.0f;
        const EngineStatus fin = engine.finish(text, confidence);
        if (!fin.ok()) { endListen(P1_VOICE_EVENT_ERROR, req.id, fin.code, "asr: " + fin.message); return; }
        if (listenCancel.load()) { endListen(P1_VOICE_EVENT_LISTEN_CANCELLED, req.id); return; }
        if (text.empty()) {
            endListen(P1_VOICE_EVENT_ERROR, req.id, timedOut ? P1_VOICE_ERR_TIMEOUT : P1_VOICE_ERR_NO_SPEECH,
                 timedOut ? "识别超时，未听到语音" : "没有识别到语音");
            return;
        }
        endListen(P1_VOICE_EVENT_LISTEN_RESULT, req.id, 0, text, confidence);
    }

    void cancelSpeak(std::uint64_t id) {
        std::lock_guard<std::mutex> lock(ttsMutex);
        for (auto it = speakQueue.begin(); it != speakQueue.end();) {
            if (id == 0 || it->id == id) {
                post(P1_VOICE_EVENT_SPEAK_CANCELLED, it->id);
                it = speakQueue.erase(it);
            } else {
                ++it;
            }
        }
        if (currentSpeak != 0 && (id == 0 || id == currentSpeak)) {
            speakCancel = true;
            if (ttsEngine) ttsEngine->interrupt();
        }
    }

    bool cancelListen(std::uint64_t id) {
        std::lock_guard<std::mutex> lock(asrMutex);
        bool hit = false;
        if (pendingListen && (id == 0 || pendingListen->id == id)) {
            post(P1_VOICE_EVENT_LISTEN_CANCELLED, pendingListen->id);
            pendingListen.reset();
            hit = true;
        }
        if (currentListen != 0 && (id == 0 || id == currentListen)) {
            listenCancel = true;
            hit = true;
        }
        return hit;
    }
};

extern "C" {

const char* p1_voice_backends(void) { return compiledBackends(); }
uint32_t p1_voice_abi_version(void) { return P1_VOICE_ABI_VERSION; }

const char* p1_voice_error_string(int32_t code) {
    switch (code) {
    case P1_VOICE_OK: return "成功";
    case P1_VOICE_ERR_INVALID_ARGUMENT: return "参数无效";
    case P1_VOICE_ERR_NOT_READY: return "语音服务未就绪或正在关闭";
    case P1_VOICE_ERR_BACKEND_UNAVAILABLE: return "语音后端不可用";
    case P1_VOICE_ERR_MODEL_MISSING: return "语音模型缺失";
    case P1_VOICE_ERR_PERMISSION_DENIED: return "未获得音频/麦克风访问权限";
    case P1_VOICE_ERR_DEVICE: return "音频设备不可用";
    case P1_VOICE_ERR_BUSY: return "正在识别中";
    case P1_VOICE_ERR_TIMEOUT: return "超时";
    case P1_VOICE_ERR_IN_CALLBACK: return "不能在语音回调中销毁适配器";
    case P1_VOICE_ERR_NO_SPEECH: return "没有识别到语音";
    case P1_VOICE_ERR_INTERNAL: return "内部错误";
    default: return "未知错误";
    }
}

P1VoiceAdapter* p1_voice_create(const P1VoiceConfig* config, P1VoiceCallback callback, void* userData,
                                int32_t* outError) {
    auto fail = [&](int32_t code) -> P1VoiceAdapter* {
        if (outError) *outError = code;
        return nullptr;
    };
    if (config && config->struct_size < sizeof(uint32_t)) return fail(P1_VOICE_ERR_INVALID_ARGUMENT);
    auto a = std::make_unique<P1VoiceAdapter>();
    VoiceSettings& s = a->settings;
    s.ttsBackend = (config && config->tts_backend && *config->tts_backend) ? config->tts_backend : defaultTtsBackend();
    s.asrBackend = (config && config->asr_backend && *config->asr_backend) ? config->asr_backend : defaultAsrBackend();
    s.language = (config && config->language && *config->language) ? config->language : "zh-CN";
    if (s.language.rfind("zh", 0) != 0) return fail(P1_VOICE_ERR_INVALID_ARGUMENT);  // 只支持普通话
    s.resourceDir = resolveResourceDir(config ? config->resource_dir : nullptr);
    if (config && config->output_wav_path) s.outputWavPath = config->output_wav_path;
    if (config && config->input_wav_path) s.inputWavPath = config->input_wav_path;
    s.inputRealtime = config && config->input_realtime != 0;
    s.speechRate = config ? config->speech_rate : 0;
    if (s.speechRate < -100 || s.speechRate > 100) return fail(P1_VOICE_ERR_INVALID_ARGUMENT);
    a->callback = callback;
    a->userData = userData;
    P1VoiceAdapter* raw = a.release();
    raw->dispatcher = std::thread([raw] { raw->dispatchLoop(); });
    raw->ttsThread = std::thread([raw] { raw->ttsLoop(); });
    raw->asrThread = std::thread([raw] { raw->asrLoop(); });
    if (outError) *outError = P1_VOICE_OK;
    return raw;
}

int32_t p1_voice_destroy(P1VoiceAdapter* a) {
    if (!a) return P1_VOICE_ERR_INVALID_ARGUMENT;
    if (std::this_thread::get_id() == a->dispatcherId.load()) return P1_VOICE_ERR_IN_CALLBACK;
    if (a->closing.exchange(true)) return P1_VOICE_ERR_NOT_READY;  // 重复销毁
    a->cancelSpeak(0);
    a->cancelListen(0);
    {
        std::lock_guard<std::mutex> lock(a->ttsMutex);
        a->ttsStop = true;
    }
    a->ttsCv.notify_all();
    {
        std::lock_guard<std::mutex> lock(a->asrMutex);
        a->asrStop = true;
    }
    a->asrCv.notify_all();
    a->ttsThread.join();
    a->asrThread.join();
    {
        std::lock_guard<std::mutex> lock(a->evMutex);
        a->evStop = true;  // 已排队的取消事件先送达，再退出
    }
    a->evCv.notify_all();
    a->dispatcher.join();
    delete a;
    return P1_VOICE_OK;
}

int32_t p1_voice_speak(P1VoiceAdapter* a, const char* text, uint32_t flags, uint64_t* outId) {
    if (outId) *outId = 0;
    if (!a || !text || !*text) return P1_VOICE_ERR_INVALID_ARGUMENT;
    if (a->closing.load()) return P1_VOICE_ERR_NOT_READY;
    {
        std::lock_guard<std::mutex> lock(a->initMutex);
        if (a->ttsInit == kInitFailed) return a->ttsStatus.code;
    }
    if (flags & P1_VOICE_SPEAK_INTERRUPT) a->cancelSpeak(0);
    const std::uint64_t id = a->nextId++;
    {
        std::lock_guard<std::mutex> lock(a->ttsMutex);
        a->speakQueue.push_back({id, text});
    }
    a->ttsCv.notify_one();
    if (outId) *outId = id;
    return P1_VOICE_OK;
}

int32_t p1_voice_listen(P1VoiceAdapter* a, uint32_t timeoutMs, uint64_t* outId) {
    if (outId) *outId = 0;
    if (!a) return P1_VOICE_ERR_INVALID_ARGUMENT;
    if (a->closing.load()) return P1_VOICE_ERR_NOT_READY;
    {
        std::lock_guard<std::mutex> lock(a->initMutex);
        if (a->asrInit == kInitFailed) return a->asrStatus.code;
    }
    const std::uint64_t id = a->nextId++;
    {
        std::lock_guard<std::mutex> lock(a->asrMutex);
        if (a->pendingListen || a->currentListen != 0) return P1_VOICE_ERR_BUSY;
        a->pendingListen = ListenRequest{id, timeoutMs};
    }
    a->asrCv.notify_one();
    if (outId) *outId = id;
    return P1_VOICE_OK;
}

int32_t p1_voice_stop_listening(P1VoiceAdapter* a) {
    if (!a) return P1_VOICE_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(a->asrMutex);
    if (a->currentListen == 0) return a->pendingListen ? P1_VOICE_OK : P1_VOICE_ERR_NOT_READY;
    a->listenStop = true;
    return P1_VOICE_OK;
}

int32_t p1_voice_cancel(P1VoiceAdapter* a, uint64_t id) {
    if (!a) return P1_VOICE_ERR_INVALID_ARGUMENT;
    a->cancelSpeak(id);
    a->cancelListen(id);
    return P1_VOICE_OK;
}

int32_t p1_voice_check_microphone(char* textOut, uint32_t capacity) {
    std::string name;
    const int r = probeMicrophone(name);
    if (textOut && capacity > 0) {
        const std::size_t n = std::min<std::size_t>(name.size(), capacity - 1);
        std::memcpy(textOut, name.data(), n);
        textOut[n] = '\0';
    }
    return r;
}

}  // extern "C"
