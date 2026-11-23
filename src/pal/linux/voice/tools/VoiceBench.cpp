// p1-voice-bench：经 VoiceFfi.h 的 C ABI 测量播报与识别（DEC-20261123-1-voice-backend-01）。
//
//   p1-voice-bench corpus --tts <后端> --phrases <文件> --out <目录>
//       用指定 TTS 把每句合成为 <目录>/<后端>-NN.wav，作为 ASR 的共同输入。
//   p1-voice-bench tts --tts <后端> --phrases <文件> --runs N --csv <文件> [--wav]
//       逐句播报（默认实际播放到默认音频输出；--wav 改为写 WAV，测纯合成耗时），
//       记录 初始化、首音延迟（speak→SPEAK_STARTED）、总耗时（→FINISHED）、打断延迟（cancel→CANCELLED）。
//   p1-voice-bench asr --asr <后端> --phrases <文件> --corpus <目录> --prefix <TTS名> --runs N --csv <文件> [--realtime]
//       逐句识别 WAV，记录 初始化、识别耗时（listen→RESULT）、实时率、字错误率 CER。
//       --realtime 按实时送入音频，额外记录“音频结束→结果”的句末延迟。
// 每个命令结束打印 VmHWM（本进程峰值 RSS）与相关子进程/守护进程的峰值 RSS。
#include "contracts/VoiceFfi.h"
#include "pal/linux/voice/AudioIo.h"
#include "pal/linux/voice/VoiceText.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace p1::voice;

namespace {

std::int64_t nowUs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

struct Ev {
    int type;
    std::uint64_t id;
    int error;
    std::string text;
    std::int64_t mono;
};

struct Collector {
    std::mutex m;
    std::condition_variable cv;
    std::deque<Ev> events;
    static void cb(const P1VoiceEvent* e, void* u) {
        auto* c = static_cast<Collector*>(u);
        std::lock_guard<std::mutex> lock(c->m);
        c->events.push_back({e->type, e->request_id, e->error_code, e->text, e->monotonic_us});
        c->cv.notify_all();
    }
    const Ev* wait(int type, std::uint64_t id, int seconds = 120) {
        std::unique_lock<std::mutex> lock(m);
        const Ev* hit = nullptr;
        cv.wait_for(lock, std::chrono::seconds(seconds), [&] {
            for (auto& e : events) if (e.type == type && e.id == id) { hit = &e; return true; }
            return false;
        });
        return hit;
    }
    const Ev* waitTerminal(std::uint64_t id, int seconds = 120) {
        std::unique_lock<std::mutex> lock(m);
        const Ev* hit = nullptr;
        cv.wait_for(lock, std::chrono::seconds(seconds), [&] {
            for (auto& e : events)
                if (e.id == id && (e.type == P1_VOICE_EVENT_SPEAK_FINISHED || e.type == P1_VOICE_EVENT_SPEAK_CANCELLED ||
                                   e.type == P1_VOICE_EVENT_LISTEN_RESULT || e.type == P1_VOICE_EVENT_LISTEN_CANCELLED ||
                                   e.type == P1_VOICE_EVENT_ERROR)) { hit = &e; return true; }
            return false;
        });
        return hit;
    }
    const Ev* find(int type, std::uint64_t id) {
        std::lock_guard<std::mutex> lock(m);
        for (auto& e : events) if (e.type == type && e.id == id) return &e;
        return nullptr;
    }
};

std::map<std::string, std::string> parseArgs(int argc, char** argv) {
    std::map<std::string, std::string> a;
    for (int i = 2; i < argc; ++i) {
        std::string k = argv[i];
        if (k.rfind("--", 0) != 0) continue;
        k = k.substr(2);
        if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) a[k] = argv[++i];
        else a[k] = "1";
    }
    return a;
}

std::vector<std::string> readPhrases(const std::string& path) {
    std::ifstream in(path);
    std::vector<std::string> out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        out.push_back(line);
    }
    return out;
}

long statusKb(int pid, const char* key) {
    std::ifstream in("/proc/" + (pid ? std::to_string(pid) : std::string("self")) + "/status");
    std::string line;
    while (std::getline(in, line))
        if (line.rfind(key, 0) == 0) return std::atol(line.c_str() + std::strlen(key));
    return -1;
}

std::string readFile(const std::string& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// 本进程子进程（Piper）的峰值 RSS；同时列出 speech-dispatcher（Navit spd-say 配置所用守护进程）。
std::string helperMemory() {
    std::ostringstream out;
    DIR* d = opendir("/proc");
    if (!d) return {};
    const int self = getpid();
    while (dirent* e = readdir(d)) {
        const int pid = std::atoi(e->d_name);
        if (pid <= 0) continue;
        std::string comm = readFile("/proc/" + std::to_string(pid) + "/comm");
        while (!comm.empty() && comm.back() == '\n') comm.pop_back();
        const std::string stat = readFile("/proc/" + std::to_string(pid) + "/stat");
        const auto rp = stat.rfind(')');
        int ppid = 0;
        if (rp != std::string::npos) std::sscanf(stat.c_str() + rp + 2, "%*c %d", &ppid);
        if (ppid == self || comm == "speech-dispatch" || comm == "sd_espeak-ng") {
            out << comm << "(" << pid << ") VmHWM=" << statusKb(pid, "VmHWM:") / 1024.0 << "MiB; ";
        }
    }
    closedir(d);
    return out.str();
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    const double idx = p * (v.size() - 1);
    const auto lo = static_cast<std::size_t>(idx);
    const auto hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (idx - lo);
}

void summary(const char* name, const std::vector<double>& v, const char* unit) {
    std::printf("  %-28s n=%zu median=%.2f p95=%.2f min=%.2f max=%.2f %s\n", name, v.size(), percentile(v, 0.5),
                percentile(v, 0.95), v.empty() ? -1 : *std::min_element(v.begin(), v.end()),
                v.empty() ? -1 : *std::max_element(v.begin(), v.end()), unit);
}

// ── CER：去标点/空白，阿拉伯数字转汉字，按码点编辑距离 ───────────────────────
std::string chineseNumber(long n) {
    static const char* digit[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
    if (n == 0) return digit[0];
    static const char* unit[] = {"", "十", "百", "千", "万"};
    std::string out;
    std::vector<int> ds;
    for (long x = n; x > 0; x /= 10) ds.push_back(static_cast<int>(x % 10));
    bool zero = false;
    for (int i = static_cast<int>(ds.size()) - 1; i >= 0; --i) {
        if (ds[i] == 0) { zero = true; continue; }
        if (zero && !out.empty()) out += digit[0];
        zero = false;
        if (!(ds[i] == 1 && i == 1 && out.empty())) out += digit[ds[i]];
        out += unit[std::min(i, 4)];
    }
    return out;
}

std::vector<std::uint32_t> normalize(const std::string& s) {
    std::string expanded;
    const auto cps = utf8Codepoints(s);
    for (std::size_t i = 0; i < cps.size();) {
        if (cps[i] >= '0' && cps[i] <= '9') {
            long n = 0;
            while (i < cps.size() && cps[i] >= '0' && cps[i] <= '9') n = n * 10 + (cps[i++] - '0');
            expanded += chineseNumber(n);
            continue;
        }
        appendUtf8(expanded, cps[i++]);
    }
    std::vector<std::uint32_t> out;
    for (std::uint32_t c : utf8Codepoints(expanded))
        if (isCjk(c) && c != 0x3002 && c != 0xFF0C) out.push_back(c);  // 只保留汉字
    return out;
}

double cer(const std::string& ref, const std::string& hyp, int& edits, int& refLen) {
    const auto r = normalize(ref), h = normalize(hyp);
    std::vector<int> prev(h.size() + 1), cur(h.size() + 1);
    for (std::size_t j = 0; j <= h.size(); ++j) prev[j] = static_cast<int>(j);
    for (std::size_t i = 1; i <= r.size(); ++i) {
        cur[0] = static_cast<int>(i);
        for (std::size_t j = 1; j <= h.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (r[i - 1] == h[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    edits = prev[h.size()];
    refLen = static_cast<int>(r.size());
    return refLen ? static_cast<double>(edits) / refLen : 0.0;
}

P1VoiceAdapter* create(const char* tts, const char* asr, const std::string& outWav, const std::string& inWav,
                       bool realtime, Collector& c, double& initMs, bool measureAsr) {
    P1VoiceConfig cfg{};
    cfg.struct_size = sizeof cfg;
    cfg.tts_backend = tts;
    cfg.asr_backend = asr;
    cfg.output_wav_path = outWav.empty() ? nullptr : outWav.c_str();
    cfg.input_wav_path = inWav.empty() ? nullptr : inWav.c_str();
    cfg.input_realtime = realtime ? 1 : 0;
    int err = 0;
    const std::int64_t t0 = nowUs();
    P1VoiceAdapter* a = p1_voice_create(&cfg, &Collector::cb, &c, &err);
    if (!a) { std::fprintf(stderr, "create failed: %s\n", p1_voice_error_string(err)); return nullptr; }
    // 被测子系统的初始化完成时刻（TTS 基准看 tts=，ASR 基准看 asr=）。
    const std::string want = std::string(measureAsr ? "asr=" : "tts=");
    std::int64_t readyMono = -1;
    {
        std::unique_lock<std::mutex> lock(c.m);
        c.cv.wait_for(lock, std::chrono::seconds(120), [&] {
            for (auto& e : c.events)
                if ((e.type == P1_VOICE_EVENT_SERVICE_READY || e.type == P1_VOICE_EVENT_SERVICE_ERROR) &&
                    e.text.rfind(want, 0) == 0) { readyMono = e.type == P1_VOICE_EVENT_SERVICE_READY ? e.mono : -1; return true; }
            return false;
        });
    }
    initMs = readyMono > 0 ? (readyMono - t0) / 1000.0 : -1;
    std::lock_guard<std::mutex> lock(c.m);
    for (auto& e : c.events)
        if (e.type == P1_VOICE_EVENT_SERVICE_ERROR) std::fprintf(stderr, "service error: %s\n", e.text.c_str());
    return a;
}

int cmdCorpus(std::map<std::string, std::string>& a) {
    const auto phrases = readPhrases(a["phrases"]);
    int n = 0;
    for (std::size_t i = 0; i < phrases.size(); ++i) {
        char name[256];
        std::snprintf(name, sizeof name, "%s/%s-%02zu.wav", a["out"].c_str(), a["tts"].c_str(), i);
        Collector c;
        double initMs = 0;
        P1VoiceAdapter* ad = create(a["tts"].c_str(), "none", name, {}, false, c, initMs, false);
        if (!ad) return 1;
        std::uint64_t id = 0;
        p1_voice_speak(ad, phrases[i].c_str(), 0, &id);
        const Ev* t = c.waitTerminal(id);
        if (t && t->type == P1_VOICE_EVENT_SPEAK_FINISHED) ++n;
        p1_voice_destroy(ad);
    }
    std::printf("corpus %s: %d/%zu wav\n", a["tts"].c_str(), n, phrases.size());
    return n == static_cast<int>(phrases.size()) ? 0 : 1;
}

int cmdTts(std::map<std::string, std::string>& a) {
    const auto phrases = readPhrases(a["phrases"]);
    const int runs = std::max(1, std::atoi(a["runs"].c_str()));
    const bool wav = a.count("wav") > 0;
    char tmpl[] = "/tmp/p1-voice-bench-XXXXXX";
    const std::string tmp = mkdtemp(tmpl);
    const std::string outWav = wav ? tmp + "/out.wav" : std::string();
    std::ofstream csv(a["csv"], std::ios::app);
    Collector c;
    double initMs = 0;
    P1VoiceAdapter* ad = create(a["tts"].c_str(), "none", outWav, {}, false, c, initMs, false);
    if (!ad) return 1;
    std::printf("tts=%s mode=%s init_ms=%.1f\n", a["tts"].c_str(), wav ? "wav" : "playback", initMs);
    csv << "tts," << a["tts"] << ",init," << (wav ? "wav" : "playback") << ",," << initMs << ",,\n";
    std::vector<double> first, total, perChar, cancelMs;
    int failures = 0;
    // 预热 1 次（不计入）
    std::uint64_t id = 0;
    p1_voice_speak(ad, "预热", 0, &id);
    c.waitTerminal(id);
    for (int r = 0; r < runs; ++r) {
        for (std::size_t i = 0; i < phrases.size(); ++i) {
            const std::int64_t t0 = nowUs();
            p1_voice_speak(ad, phrases[i].c_str(), 0, &id);
            const Ev* t = c.waitTerminal(id);
            const Ev* s = c.find(P1_VOICE_EVENT_SPEAK_STARTED, id);
            if (!t || t->type != P1_VOICE_EVENT_SPEAK_FINISHED || !s) {
                ++failures;
                csv << "tts," << a["tts"] << ",speak_fail," << r << "," << i << ",,,\n";
                continue;
            }
            const double f = (s->mono - t0) / 1000.0, tot = (t->mono - t0) / 1000.0;
            first.push_back(f);
            total.push_back(tot);
            const double chars = static_cast<double>(utf8Codepoints(phrases[i]).size());
            perChar.push_back(tot / chars);
            csv << "tts," << a["tts"] << ",speak," << r << "," << i << "," << f << "," << tot << "," << chars << "\n";
        }
    }
    // 打断延迟：长句开始发声 300 ms 后取消（仅实际播放模式有意义）。
    if (!wav) {
        for (int r = 0; r < std::max(3, runs); ++r) {
            p1_voice_speak(ad, "沿当前道路继续行驶两公里，然后在第三个路口向左转进入长江大桥方向，注意保持车道", 0, &id);
            if (!c.wait(P1_VOICE_EVENT_SPEAK_STARTED, id, 30)) { ++failures; continue; }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            const std::int64_t t0 = nowUs();
            p1_voice_cancel(ad, id);
            const Ev* t = c.waitTerminal(id, 30);
            if (t && t->type == P1_VOICE_EVENT_SPEAK_CANCELLED) {
                cancelMs.push_back((t->mono - t0) / 1000.0);
                csv << "tts," << a["tts"] << ",cancel," << r << ",," << cancelMs.back() << ",,\n";
            } else {
                ++failures;
            }
        }
    }
    summary("first_audio_ms", first, "ms");
    summary("total_ms", total, "ms");
    summary("total_ms_per_char", perChar, "ms/char");
    if (!wav) summary("cancel_ms", cancelMs, "ms");
    std::printf("  failures=%d\n", failures);
    const std::string helpers = helperMemory();
    std::printf("  self VmHWM=%.1f MiB; %s\n", statusKb(0, "VmHWM:") / 1024.0, helpers.c_str());
    csv << "tts," << a["tts"] << ",mem," << (wav ? "wav" : "playback") << ",," << statusKb(0, "VmHWM:") / 1024.0
        << ",\"" << helpers << "\",\n";
    const std::int64_t d0 = nowUs();
    p1_voice_destroy(ad);
    std::printf("  destroy_ms=%.1f\n", (nowUs() - d0) / 1000.0);
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    return 0;
}

int cmdAsr(std::map<std::string, std::string>& a) {
    const auto phrases = readPhrases(a["phrases"]);
    const int runs = std::max(1, std::atoi(a["runs"].c_str()));
    const bool realtime = a.count("realtime") > 0;
    std::ofstream csv(a["csv"], std::ios::app);
    std::vector<double> latency, rtf, endLatency;
    int edits = 0, refChars = 0, failures = 0, exact = 0, total = 0;
    double initMs = -1;
    double maxHwm = 0;
    for (std::size_t i = 0; i < phrases.size(); ++i) {
        char wav[512];
        std::snprintf(wav, sizeof wav, "%s/%s-%02zu.wav", a["corpus"].c_str(), a["prefix"].c_str(), i);
        WavData w;
        std::string err;
        if (!readWav(wav, w, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        const double audioMs = 1000.0 * (w.samples.size() / w.channels) / w.sampleRate;
        // 每句一个适配器：输入 WAV 在创建时指定；模型加载只计入 init，不计入识别耗时。
        Collector c;
        double im = 0;
        P1VoiceAdapter* ad = create("none", a["asr"].c_str(), {}, wav, realtime, c, im, true);
        if (!ad) return 1;
        if (i == 0) initMs = im;
        for (int r = 0; r < runs; ++r) {
            std::uint64_t id = 0;
            const std::int64_t t0 = nowUs();
            p1_voice_listen(ad, 30000, &id);
            const Ev* t = c.waitTerminal(id);
            ++total;
            if (!t || t->type != P1_VOICE_EVENT_LISTEN_RESULT) {
                ++failures;
                csv << "asr," << a["asr"] << "," << a["prefix"] << "," << r << "," << i << ",fail,"
                    << (t ? t->error : 0) << ",\n";
                int e = 0, n = 0;
                cer(phrases[i], "", e, n);
                edits += e;
                refChars += n;
                continue;
            }
            const double ms = (t->mono - t0) / 1000.0;
            int e = 0, n = 0;
            const double c1 = cer(phrases[i], t->text, e, n);
            edits += e;
            refChars += n;
            if (e == 0) ++exact;
            if (realtime) {
                endLatency.push_back(ms - audioMs);
            } else {
                latency.push_back(ms);
                rtf.push_back(ms / audioMs);
            }
            csv << "asr," << a["asr"] << "," << a["prefix"] << "," << r << "," << i << "," << ms << "," << audioMs
                << "," << c1 << ",\"" << t->text << "\"\n";
            if (r == 0) std::printf("  [%02zu] %-24s -> %s (CER %.2f)\n", i, phrases[i].c_str(), t->text.c_str(), c1);
        }
        maxHwm = std::max(maxHwm, statusKb(0, "VmHWM:") / 1024.0);
        p1_voice_destroy(ad);
    }
    std::printf("asr=%s corpus=%s mode=%s init_ms(first)=%.1f\n", a["asr"].c_str(), a["prefix"].c_str(),
                realtime ? "realtime" : "fast", initMs);
    if (realtime) summary("end_of_audio_to_result_ms", endLatency, "ms");
    else { summary("recognize_ms", latency, "ms"); summary("rtf", rtf, "x"); }
    std::printf("  CER=%.4f (%d/%d) exact=%d/%d failures=%d self VmHWM=%.1f MiB\n",
                refChars ? double(edits) / refChars : 0.0, edits, refChars, exact, total, failures, maxHwm);
    csv << "asr," << a["asr"] << "," << a["prefix"] << ",summary," << (realtime ? "realtime" : "fast") << ","
        << initMs << "," << (refChars ? double(edits) / refChars : 0.0) << "," << maxHwm << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: p1-voice-bench corpus|tts|asr ...（见源码头注释）\n");
        return 2;
    }
    auto args = parseArgs(argc, argv);
    const std::string cmd = argv[1];
    if (cmd == "corpus") return cmdCorpus(args);
    if (cmd == "tts") return cmdTts(args);
    if (cmd == "asr") return cmdAsr(args);
    std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
