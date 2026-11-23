// 引擎工厂。后端按 DEC-20261123-1-voice-backend-01 最终选定：TTS = Piper，ASR = Vosk。
// 名称 "none" 等未知后端返回空指针，对应子系统报告 BACKEND_UNAVAILABLE（只用一侧时使用）。
#include "pal/linux/voice/VoiceEngine.h"

namespace p1::voice {

std::unique_ptr<TtsEngine> makePiperTts();
std::unique_ptr<AsrEngine> makeVoskAsr();

std::unique_ptr<TtsEngine> createTtsEngine(const std::string& b) {
    return b == "piper" ? makePiperTts() : nullptr;
}

std::unique_ptr<AsrEngine> createAsrEngine(const std::string& b) {
    return b == "vosk" ? makeVoskAsr() : nullptr;
}

const char* compiledBackends() { return "tts=piper,;asr=vosk,"; }
const char* defaultTtsBackend() { return "piper"; }
const char* defaultAsrBackend() { return "vosk"; }

}  // namespace p1::voice
