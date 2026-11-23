// ASR：Vosk（Kaldi）C API + vosk-model-small-cn-0.22 普通话小模型。
// 流式识别：每 100 ms 送入音频，可给出中间结果；accept_waveform 返回 1 表示 Kaldi 端点检测判定句末。
#include "pal/linux/voice/VoiceEngine.h"
#include "pal/linux/voice/VoiceText.h"

#include "contracts/VoiceFfi.h"

#include <vosk_api.h>

#include <filesystem>
#include <mutex>

namespace p1::voice {
namespace {

class VoskAsr final : public AsrEngine {
public:
    ~VoskAsr() override {
        if (rec_) vosk_recognizer_free(rec_);
        if (model_) vosk_model_free(model_);
    }
    const char* name() const override { return "vosk"; }

    EngineStatus init(const VoiceSettings& s) override {
        const std::filesystem::path dir = std::filesystem::path(s.resourceDir) / "vosk-model-small-cn-0.22";
        if (!std::filesystem::exists(dir / "am" / "final.mdl"))
            return {P1_VOICE_ERR_MODEL_MISSING, "缺少 Vosk 中文模型: " + dir.string()};
        static std::once_flag logOnce;
        std::call_once(logOnce, [] { vosk_set_log_level(-1); });
        model_ = vosk_model_new(dir.c_str());
        if (!model_) return {P1_VOICE_ERR_MODEL_MISSING, "Vosk 模型加载失败: " + dir.string()};
        rec_ = vosk_recognizer_new(model_, 16000.0f);
        if (!rec_) return {P1_VOICE_ERR_INTERNAL, "Vosk 识别器创建失败"};
        return {};
    }

    EngineStatus begin() override {
        vosk_recognizer_reset(rec_);
        lastPartial_.clear();
        endpointText_.clear();
        endpointHit_ = false;
        return {};
    }

    EngineStatus feed(const int16_t* pcm, std::size_t n, std::string& partial, bool& endpoint) override {
        const int r = vosk_recognizer_accept_waveform_s(rec_, reinterpret_cast<const short*>(pcm), static_cast<int>(n));
        if (r < 0) return {P1_VOICE_ERR_INTERNAL, "Vosk 处理音频失败"};
        if (r == 1) {
            const std::string text = joinChinese(jsonStringField(vosk_recognizer_result(rec_), "text"));
            if (!text.empty()) {  // 空的端点（开头静音）继续等
                endpointText_ = text;
                endpointHit_ = true;
                endpoint = true;
            }
            return {};
        }
        const std::string p = joinChinese(jsonStringField(vosk_recognizer_partial_result(rec_), "partial"));
        if (!p.empty() && p != lastPartial_) { lastPartial_ = p; partial = p; }
        return {};
    }

    EngineStatus finish(std::string& text, float& confidence) override {
        text = endpointHit_ ? endpointText_
                            : joinChinese(jsonStringField(vosk_recognizer_final_result(rec_), "text"));
        confidence = -1.0f;  // 未启用逐词置信度
        return {};
    }

private:
    VoskModel* model_ = nullptr;
    VoskRecognizer* rec_ = nullptr;
    std::string lastPartial_;
    std::string endpointText_;
    bool endpointHit_ = false;
};

}  // namespace

std::unique_ptr<AsrEngine> makeVoskAsr() { return std::make_unique<VoskAsr>(); }

}  // namespace p1::voice
