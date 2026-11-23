// LinuxVoiceAdapter 行为与生命周期测试：只经 VoiceFfi.h 的 C ABI 调用，与核心层用法一致。
// 不需要声卡：TTS 写 WAV，ASR 读 WAV（由 TTS 合成的中文指令或静音）。
// “播报中销毁”实际播放到默认音频输出（本虚拟机为 PipeWire auto_null，按实时推进）。
#include "contracts/VoiceFfi.h"
#include "pal/linux/voice/AudioIo.h"
#include "pal/linux/voice/VoiceText.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <deque>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

int gFailures = 0;
int gChecks = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        ++gChecks;                                                                  \
        if (!(cond)) {                                                              \
            ++gFailures;                                                            \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

struct Event {
    int type;
    std::uint64_t id;
    int error;
    std::string text;
};

struct Recorder {
    std::mutex m;
    std::condition_variable cv;
    std::deque<Event> events;  // deque：追加不使已取得的元素指针失效
    std::atomic<int> afterDestroy{0};
    std::atomic<bool> destroyed{false};
    std::function<void(const P1VoiceEvent*)> hook;

    static void cb(const P1VoiceEvent* e, void* u) {
        auto* r = static_cast<Recorder*>(u);
        if (r->destroyed) r->afterDestroy++;
        if (r->hook) r->hook(e);
        std::lock_guard<std::mutex> lock(r->m);
        r->events.push_back({e->type, e->request_id, e->error_code, e->text ? e->text : ""});
        r->cv.notify_all();
    }

    bool waitFor(const std::function<bool(const std::deque<Event>&)>& pred, std::chrono::seconds t = 60s) {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, t, [&] { return pred(events); });
    }
    bool waitType(int type, std::uint64_t id, std::chrono::seconds t = 60s) {
        return waitFor([&](const std::deque<Event>& ev) {
            for (auto& e : ev) if (e.type == type && (id == 0 || e.id == id)) return true;
            return false;
        }, t);
    }
    bool waitTerminal(std::uint64_t id, std::chrono::seconds t = 90s) {
        return waitFor([&](const std::deque<Event>& ev) {
            for (auto& e : ev)
                if (e.id == id && (e.type == P1_VOICE_EVENT_SPEAK_FINISHED || e.type == P1_VOICE_EVENT_SPEAK_CANCELLED ||
                                   e.type == P1_VOICE_EVENT_LISTEN_RESULT || e.type == P1_VOICE_EVENT_LISTEN_CANCELLED ||
                                   e.type == P1_VOICE_EVENT_ERROR))
                    return true;
            return false;
        }, t);
    }
    const Event* find(int type, std::uint64_t id) {
        std::lock_guard<std::mutex> lock(m);
        for (auto& e : events) if (e.type == type && e.id == id) return &e;
        return nullptr;
    }
    int indexOf(int type, std::uint64_t id) {
        std::lock_guard<std::mutex> lock(m);
        for (std::size_t i = 0; i < events.size(); ++i)
            if (events[i].type == type && events[i].id == id) return static_cast<int>(i);
        return -1;
    }
};

struct Adapter {
    Recorder rec;
    P1VoiceAdapter* a = nullptr;
    int createError = 0;
    Adapter(const char* tts, const char* asr, const std::string& outWav = {}, const std::string& inWav = {},
            const char* resources = nullptr) {
        P1VoiceConfig c{};
        c.struct_size = sizeof c;
        c.tts_backend = tts;
        c.asr_backend = asr;
        c.output_wav_path = outWav.empty() ? nullptr : outWav.c_str();
        c.input_wav_path = inWav.empty() ? nullptr : inWav.c_str();
        c.resource_dir = resources;
        a = p1_voice_create(&c, &Recorder::cb, &rec, &createError);
    }
    ~Adapter() { close(); }
    void close() {
        if (a) { CHECK(p1_voice_destroy(a) == P1_VOICE_OK); a = nullptr; rec.destroyed = true; }
    }
    // 两个子系统都报告了初始化结果（READY 或 ERROR）。
    bool ready() {
        return rec.waitFor([](const std::deque<Event>& ev) {
            int n = 0;
            for (auto& e : ev) n += (e.type == P1_VOICE_EVENT_SERVICE_READY || e.type == P1_VOICE_EVENT_SERVICE_ERROR);
            return n >= 2;
        });
    }
};

bool backendCompiled(const std::string& name) {
    return std::string(p1_voice_backends()).find(name + ",") != std::string::npos;
}

fs::path tmpDir() {
    static fs::path d = [] {
        char t[] = "/tmp/p1-voice-tests-XXXXXX";
        return fs::path(mkdtemp(t));
    }();
    return d;
}

// 用 TTS 合成测试语音（WAV），作为 ASR 的确定性输入。
std::string synthToWav(const char* tts, const std::string& text, const std::string& name) {
    const std::string path = (tmpDir() / name).string();
    Adapter ad(tts, "none", path);
    if (!ad.a) return {};
    std::uint64_t id = 0;
    p1_voice_speak(ad.a, text.c_str(), 0, &id);
    ad.rec.waitTerminal(id);
    ad.close();
    return fs::exists(path) && fs::file_size(path) > 1000 ? path : std::string();
}

std::string silenceWav() {
    const std::string path = (tmpDir() / "silence.wav").string();
    std::string err;
    p1::voice::writeWav(path, 16000, std::vector<int16_t>(16000 * 2, 0), err);
    return path;
}

void testTtsBackend(const char* tts) {
    std::printf("[tts %s]\n", tts);
    const bool toWav = true;
    const std::string out = (tmpDir() / (std::string(tts) + ".wav")).string();
    Adapter ad(tts, "none", out);
    CHECK(ad.a != nullptr);
    if (!ad.a) return;
    // 初始化完成前提交也应排队执行
    std::uint64_t id = 0;
    CHECK(p1_voice_speak(ad.a, "前方三百米右转", 0, &id) == P1_VOICE_OK);
    CHECK(id > 0);
    CHECK(ad.ready());
    CHECK(ad.rec.waitTerminal(id));
    CHECK(ad.rec.find(P1_VOICE_EVENT_SPEAK_FINISHED, id) != nullptr);
    CHECK(ad.rec.indexOf(P1_VOICE_EVENT_SPEAK_STARTED, id) >= 0);
    CHECK(ad.rec.indexOf(P1_VOICE_EVENT_SPEAK_STARTED, id) < ad.rec.indexOf(P1_VOICE_EVENT_SPEAK_FINISHED, id));
    if (toWav) CHECK(fs::exists(out) && fs::file_size(out) > 1000);

    // 打断：长句播报中再次 speak(INTERRUPT)，旧请求 CANCELLED，排队的也取消，新请求完成。
    std::uint64_t longId = 0, queued = 0, urgent = 0;
    p1_voice_speak(ad.a, "沿当前道路继续行驶两公里，然后在第三个路口向左转进入长江大桥方向，注意保持车道", 0, &longId);
    p1_voice_speak(ad.a, "这一句会被清掉", 0, &queued);
    CHECK(ad.rec.waitType(P1_VOICE_EVENT_SPEAK_STARTED, longId, 30s) || toWav);
    CHECK(p1_voice_speak(ad.a, "您已偏离路线，正在重新规划", P1_VOICE_SPEAK_INTERRUPT, &urgent) == P1_VOICE_OK);
    CHECK(ad.rec.waitTerminal(urgent));
    CHECK(ad.rec.find(P1_VOICE_EVENT_SPEAK_FINISHED, urgent) != nullptr);
    CHECK(ad.rec.find(P1_VOICE_EVENT_SPEAK_CANCELLED, queued) != nullptr);
    // WAV 模式下长句可能在打断前已经合成完毕（不按实时播放），因此只要求有终态。
    CHECK(ad.rec.find(P1_VOICE_EVENT_SPEAK_CANCELLED, longId) || ad.rec.find(P1_VOICE_EVENT_SPEAK_FINISHED, longId));

    // 参数校验
    CHECK(p1_voice_speak(ad.a, "", 0, &id) == P1_VOICE_ERR_INVALID_ARGUMENT);
    CHECK(p1_voice_speak(ad.a, nullptr, 0, &id) == P1_VOICE_ERR_INVALID_ARGUMENT);
}

void testDestroyWhileSpeaking(const char* tts) {
    std::printf("[destroy while speaking %s]\n", tts);
    Adapter ad(tts, "none");  // 实际播放（auto_null），播放按实时推进，可以在中途销毁
    if (!ad.a) { CHECK(false); return; }
    CHECK(ad.ready());
    std::uint64_t a = 0, b = 0;
    p1_voice_speak(ad.a, "这是一段比较长的播报内容，用于验证销毁适配器时正在进行的播报会被取消并及时回收线程", 0, &a);
    p1_voice_speak(ad.a, "排队中的第二句", 0, &b);
    ad.rec.waitType(P1_VOICE_EVENT_SPEAK_STARTED, a, 20s);
    const auto t0 = std::chrono::steady_clock::now();
    ad.close();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  destroy took %lld ms\n", static_cast<long long>(ms));
    CHECK(ms < 3000);
    CHECK(ad.rec.find(P1_VOICE_EVENT_SPEAK_CANCELLED, b) != nullptr);  // 排队请求在 destroy 返回前收到取消
    CHECK(ad.rec.find(P1_VOICE_EVENT_SPEAK_CANCELLED, a) || ad.rec.find(P1_VOICE_EVENT_SPEAK_FINISHED, a));
    std::this_thread::sleep_for(300ms);
    CHECK(ad.rec.afterDestroy.load() == 0);  // destroy 返回后不再回调
}

void testAsrBackend(const char* asr, const std::string& wav, const std::string& expectContains) {
    std::printf("[asr %s]\n", asr);
    Adapter ad("none", asr, {}, wav);
    CHECK(ad.a != nullptr);
    if (!ad.a) return;
    std::uint64_t id = 0;
    CHECK(p1_voice_listen(ad.a, 15000, &id) == P1_VOICE_OK);
    std::uint64_t busy = 0;
    CHECK(p1_voice_listen(ad.a, 15000, &busy) == P1_VOICE_ERR_BUSY);  // 一次只允许一个识别
    CHECK(busy == 0);
    CHECK(ad.rec.waitTerminal(id, 120s));
    const Event* r = ad.rec.find(P1_VOICE_EVENT_LISTEN_RESULT, id);
    CHECK(r != nullptr);
    if (r) {
        std::printf("  result: %s\n", r->text.c_str());
        CHECK(r->text.find(expectContains) != std::string::npos);
    }
    CHECK(ad.rec.indexOf(P1_VOICE_EVENT_LISTEN_STARTED, id) >= 0);
    CHECK(ad.rec.indexOf(P1_VOICE_EVENT_LISTEN_STARTED, id) < ad.rec.indexOf(P1_VOICE_EVENT_LISTEN_RESULT, id));
}

void testAsrSilence(const char* asr) {
    std::printf("[asr %s silence]\n", asr);
    Adapter ad("none", asr, {}, silenceWav());
    if (!ad.a) { CHECK(false); return; }
    std::uint64_t id = 0;
    p1_voice_listen(ad.a, 5000, &id);
    CHECK(ad.rec.waitTerminal(id, 60s));
    const Event* e = ad.rec.find(P1_VOICE_EVENT_ERROR, id);
    CHECK(e != nullptr);
    if (e) CHECK(e->error == P1_VOICE_ERR_NO_SPEECH || e->error == P1_VOICE_ERR_TIMEOUT);
}

void testListenCancel(const char* asr, const std::string& wav) {
    std::printf("[asr %s cancel]\n", asr);
    P1VoiceConfig c{};
    c.struct_size = sizeof c;
    c.tts_backend = "none";
    c.asr_backend = asr;
    c.input_wav_path = wav.c_str();
    c.input_realtime = 1;  // 按实时速度送入，便于在中途取消
    Recorder rec;
    int err = 0;
    P1VoiceAdapter* a = p1_voice_create(&c, &Recorder::cb, &rec, &err);
    CHECK(a != nullptr);
    if (!a) return;
    std::uint64_t id = 0;
    p1_voice_listen(a, 15000, &id);
    CHECK(rec.waitType(P1_VOICE_EVENT_LISTEN_STARTED, id, 60s));
    CHECK(p1_voice_cancel(a, id) == P1_VOICE_OK);
    CHECK(rec.waitTerminal(id, 10s));
    CHECK(rec.find(P1_VOICE_EVENT_LISTEN_CANCELLED, id) != nullptr);
    CHECK(rec.find(P1_VOICE_EVENT_LISTEN_RESULT, id) == nullptr);
    CHECK(p1_voice_destroy(a) == P1_VOICE_OK);
}

// 回归：在 LISTEN_RESULT 回调里立即发起下一次识别必须被接受（曾因先发事件后清“在途”标记而返回 BUSY）。
void testListenBackToBack(const std::string& wav) {
    std::printf("[asr back-to-back listen]\n");
    Adapter ad("none", "vosk", {}, wav);
    if (!ad.a) { CHECK(false); return; }
    std::atomic<int> rc{1};
    std::atomic<std::uint64_t> second{0};
    P1VoiceAdapter* raw = ad.a;
    ad.rec.hook = [&](const P1VoiceEvent* e) {
        if (e->type == P1_VOICE_EVENT_LISTEN_RESULT && second.load() == 0) {
            std::uint64_t id = 0;
            rc = p1_voice_listen(raw, 15000, &id);
            second = id ? id : ~0ull;
        }
    };
    std::uint64_t first = 0;
    CHECK(p1_voice_listen(ad.a, 15000, &first) == P1_VOICE_OK);
    CHECK(ad.rec.waitTerminal(first, 60s));
    CHECK(ad.rec.waitFor([&](const std::deque<Event>&) { return second.load() != 0; }, 10s));
    CHECK(rc.load() == P1_VOICE_OK);
    if (rc.load() == P1_VOICE_OK) {
        CHECK(ad.rec.waitTerminal(second.load(), 60s));
        CHECK(ad.rec.find(P1_VOICE_EVENT_LISTEN_RESULT, second.load()) != nullptr);
    }
    ad.close();
    ad.rec.hook = nullptr;
}

void testMissingModel() {
    std::printf("[missing model]\n");
    // 资源目录只含 Piper 与音色、缺少 Vosk 模型：识别不可用，播报仍可用。
    const fs::path partial = tmpDir() / "no-asr-model";
    fs::create_directories(partial);
    const fs::path real = fs::path(P1_VOICE_TEST_RESOURCES);
    fs::create_directory_symlink(real / "piper", partial / "piper");
    fs::create_directory_symlink(real / "piper-voice", partial / "piper-voice");
    const std::string empty = partial.string();
    Adapter ad("piper", "vosk", (tmpDir() / "degraded.wav").string(), {}, empty.c_str());
    CHECK(ad.a != nullptr);
    if (!ad.a) return;
    CHECK(ad.ready());
    bool sawError = false;
    {
        std::lock_guard<std::mutex> lock(ad.rec.m);
        for (auto& e : ad.rec.events)
            if (e.type == P1_VOICE_EVENT_SERVICE_ERROR && e.error == P1_VOICE_ERR_MODEL_MISSING) sawError = true;
    }
    CHECK(sawError);
    std::uint64_t id = 0;
    CHECK(p1_voice_listen(ad.a, 1000, &id) == P1_VOICE_ERR_MODEL_MISSING);
    // TTS 不受影响（降级：识别不可用但播报可用）
    CHECK(p1_voice_speak(ad.a, "识别模型缺失", 0, &id) == P1_VOICE_OK);
}

void testDestroyInCallback() {
    std::printf("[destroy in callback]\n");
    const std::string out = (tmpDir() / "cb.wav").string();
    Adapter ad("piper", "none", out);
    std::atomic<int> rc{1};
    P1VoiceAdapter* raw = ad.a;
    ad.rec.hook = [&](const P1VoiceEvent* e) {
        if (e->type == P1_VOICE_EVENT_SPEAK_FINISHED) rc = p1_voice_destroy(raw);
    };
    std::uint64_t id = 0;
    p1_voice_speak(ad.a, "测试", 0, &id);
    CHECK(ad.rec.waitTerminal(id));
    std::this_thread::sleep_for(100ms);
    CHECK(rc.load() == P1_VOICE_ERR_IN_CALLBACK);
    ad.rec.hook = nullptr;
}

void testArguments() {
    std::printf("[arguments / unknown backends]\n");
    P1VoiceConfig c{};
    c.struct_size = sizeof c;
    c.tts_backend = "piper";
    c.language = "en-US";
    int err = 0;
    CHECK(p1_voice_create(&c, nullptr, nullptr, &err) == nullptr);  // 只支持普通话
    CHECK(err == P1_VOICE_ERR_INVALID_ARGUMENT);
    c.language = nullptr;
    c.speech_rate = 200;
    CHECK(p1_voice_create(&c, nullptr, nullptr, &err) == nullptr);
    CHECK(err == P1_VOICE_ERR_INVALID_ARGUMENT);
    CHECK(p1_voice_destroy(nullptr) == P1_VOICE_ERR_INVALID_ARGUMENT);
    Adapter unknown("no-such-tts", "no-such-asr");
    CHECK(unknown.a != nullptr);
    CHECK(unknown.ready());
    std::uint64_t id = 0;
    CHECK(p1_voice_speak(unknown.a, "x", 0, &id) == P1_VOICE_ERR_BACKEND_UNAVAILABLE);
    CHECK(p1_voice_listen(unknown.a, 1000, &id) == P1_VOICE_ERR_BACKEND_UNAVAILABLE);
    // 同进程两个实例互不干扰（各自的 Piper 子进程）
    Adapter a1("piper", "none", (tmpDir() / "p1.wav").string());
    Adapter a2("piper", "none", (tmpDir() / "p2.wav").string());
    std::uint64_t i1 = 0, i2 = 0;
    CHECK(p1_voice_speak(a1.a, "第一个实例", 0, &i1) == P1_VOICE_OK);
    CHECK(p1_voice_speak(a2.a, "第二个实例", 0, &i2) == P1_VOICE_OK);
    CHECK(a1.rec.waitTerminal(i1) && a1.rec.find(P1_VOICE_EVENT_SPEAK_FINISHED, i1));
    CHECK(a2.rec.waitTerminal(i2) && a2.rec.find(P1_VOICE_EVENT_SPEAK_FINISHED, i2));
}

void testMicrophoneUnavailable() {
    std::printf("[microphone permission/device]\n");
    char name[256] = {};
    const int real = p1_voice_check_microphone(name, sizeof name);
    std::printf("  default source: %s -> %s\n", name, p1_voice_error_string(real));
    CHECK(real == P1_VOICE_OK || real == P1_VOICE_ERR_DEVICE || real == P1_VOICE_ERR_PERMISSION_DENIED);
    // 模拟沙箱未授予音频访问：音频服务不可达时，listen 以错误事件结束而不是崩溃或挂起。
    const char* old = std::getenv("PULSE_SERVER");
    const std::string saved = old ? old : "";
    setenv("PULSE_SERVER", "unix:/nonexistent/p1-denied", 1);
    CHECK(p1_voice_check_microphone(name, sizeof name) != P1_VOICE_OK);
    {
        Adapter ad("none", "vosk");
        if (ad.a) {
            std::uint64_t id = 0;
            CHECK(p1_voice_listen(ad.a, 2000, &id) == P1_VOICE_OK);
            CHECK(ad.rec.waitTerminal(id, 60s));
            const Event* e = ad.rec.find(P1_VOICE_EVENT_ERROR, id);
            CHECK(e != nullptr);
            if (e) CHECK(e->error == P1_VOICE_ERR_DEVICE || e->error == P1_VOICE_ERR_PERMISSION_DENIED);
        }
    }
    if (old) setenv("PULSE_SERVER", saved.c_str(), 1); else unsetenv("PULSE_SERVER");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("backends: %s\n", p1_voice_backends());
    testArguments();
    testMissingModel();
    testDestroyInCallback();
    testMicrophoneUnavailable();
    CHECK(backendCompiled("piper") && backendCompiled("vosk"));
    testTtsBackend("piper");
    testDestroyWhileSpeaking("piper");
    const std::string cmd = synthToWav("piper", "开始导航", "cmd-piper.wav");
    CHECK(!cmd.empty());
    if (!cmd.empty()) {
        testAsrBackend("vosk", cmd, "开始导航");
        testAsrSilence("vosk");
        testListenCancel("vosk", cmd);
        for (int i = 0; i < 3; ++i) testListenBackToBack(cmd);
    }
    std::error_code ec;
    fs::remove_all(tmpDir(), ec);
    std::printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
