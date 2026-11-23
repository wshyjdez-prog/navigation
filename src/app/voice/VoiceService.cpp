#include "app/voice/VoiceService.h"

#include "contracts/VoiceFfi.h"

#include <QMetaObject>

namespace p1::app {

// 回调 user_data：随适配器实例存活，destroy 返回后才释放。
struct VoiceService::CallbackContext {
    VoiceService* service;
    std::uint64_t generation;
};

VoiceService::VoiceService(QObject* parent) : QObject(parent) {
    qRegisterMetaType<std::uint64_t>("std::uint64_t");
}

VoiceService::~VoiceService() { stop(); }

bool VoiceService::start(const VoiceOptions& o) {
    if (adapter_) return true;
    options_ = o;
    ttsReady_ = asrReady_ = false;
    ++generation_;
    const QByteArray tts = o.ttsBackend.toUtf8(), asr = o.asrBackend.toUtf8(), res = o.resourceDir.toUtf8(),
                     out = o.outputWavPath.toUtf8(), in = o.inputWavPath.toUtf8();
    P1VoiceConfig c{};
    c.struct_size = sizeof c;
    c.tts_backend = tts.isEmpty() ? nullptr : tts.constData();
    c.asr_backend = asr.isEmpty() ? nullptr : asr.constData();
    c.language = "zh-CN";
    c.resource_dir = res.isEmpty() ? nullptr : res.constData();
    c.output_wav_path = out.isEmpty() ? nullptr : out.constData();
    c.input_wav_path = in.isEmpty() ? nullptr : in.constData();
    c.input_realtime = o.inputRealtime ? 1 : 0;
    c.speech_rate = o.speechRate;
    // 上下文随适配器存活：destroy 返回后才释放，回调期间始终有效。
    auto* ctx = new CallbackContext{this, generation_};
    int err = 0;
    adapter_ = p1_voice_create(&c, &VoiceService::onEvent, ctx, &err);
    if (!adapter_) {
        delete ctx;
        emit serviceStatus("tts", false, errorText(err));
        emit serviceStatus("asr", false, errorText(err));
        return false;
    }
    context_ = ctx;
    return true;
}

void VoiceService::stop() {
    if (!adapter_) return;
    P1VoiceAdapter* a = adapter_;
    adapter_ = nullptr;
    p1_voice_destroy(a);  // 主线程调用；返回后不再回调
    delete context_;
    context_ = nullptr;
    ++generation_;
    ttsReady_ = asrReady_ = false;
    if (listenId_) { listenId_ = 0; emit listeningChanged(false); }
}

std::uint64_t VoiceService::speak(const QString& text, bool interrupt) {
    if (!adapter_ && !start(options_)) return 0;  // 按需启动：首次播报时才加载引擎
    std::uint64_t id = 0;
    const int r = p1_voice_speak(adapter_, text.toUtf8().constData(), interrupt ? P1_VOICE_SPEAK_INTERRUPT : 0u, &id);
    if (r != P1_VOICE_OK) { emit errorOccurred(0, r, errorText(r)); return 0; }
    return id;
}

std::uint64_t VoiceService::listen(int timeoutMs) {
    if (!adapter_ && !start(options_)) return 0;
    std::uint64_t id = 0;
    const int r = p1_voice_listen(adapter_, static_cast<std::uint32_t>(timeoutMs), &id);
    if (r != P1_VOICE_OK) { emit errorOccurred(0, r, errorText(r)); return 0; }
    listenId_ = id;
    emit listeningChanged(true);
    return id;
}

void VoiceService::stopListening() {
    if (adapter_) p1_voice_stop_listening(adapter_);
}

void VoiceService::cancel(std::uint64_t id) {
    if (adapter_) p1_voice_cancel(adapter_, id);
}

QString VoiceService::compiledBackends() { return QString::fromUtf8(p1_voice_backends()); }
QString VoiceService::errorText(int code) { return QString::fromUtf8(p1_voice_error_string(code)); }

int VoiceService::checkMicrophone(QString* name) {
    char buf[256] = {};
    const int r = p1_voice_check_microphone(buf, sizeof buf);
    if (name) *name = QString::fromUtf8(buf);
    return r;
}

// 适配器分发线程：只复制数据并排队到主线程，不触碰任何 Qt 部件。
void VoiceService::onEvent(const P1VoiceEvent* e, void* user) {
    auto* ctx = static_cast<CallbackContext*>(user);
    const int type = e->type;
    const std::uint64_t id = e->request_id;
    const int code = e->error_code;
    const QString text = QString::fromUtf8(e->text ? e->text : "");
    const float confidence = e->confidence;
    const std::uint64_t gen = ctx->generation;
    VoiceService* self = ctx->service;  // stop() 在销毁前完成，回调期间 self 一定有效
    // 以 self 为上下文对象：若投递前 self 已销毁，Qt 丢弃该调用；generation 丢弃旧实例的残留事件。
    QMetaObject::invokeMethod(self, [self, gen, type, id, code, text, confidence] {
        if (self->generation_ == gen) self->deliver(type, id, code, text, confidence);
    }, Qt::QueuedConnection);
}

void VoiceService::deliver(int type, std::uint64_t id, int code, const QString& text, float confidence) {
    const auto endListen = [&] {
        if (id != 0 && id == listenId_) { listenId_ = 0; emit listeningChanged(false); }
    };
    switch (type) {
    case P1_VOICE_EVENT_SERVICE_READY:
    case P1_VOICE_EVENT_SERVICE_ERROR: {
        const bool ok = type == P1_VOICE_EVENT_SERVICE_READY;
        const QString subsystem = text.section('=', 0, 0);
        if (subsystem == "tts") ttsReady_ = ok;
        if (subsystem == "asr") asrReady_ = ok;
        emit serviceStatus(subsystem, ok, text.section('=', 1));
        break;
    }
    case P1_VOICE_EVENT_SPEAK_STARTED: emit speakStarted(id); break;
    case P1_VOICE_EVENT_SPEAK_FINISHED: emit speakFinished(id); break;
    case P1_VOICE_EVENT_SPEAK_CANCELLED: emit speakCancelled(id); break;
    case P1_VOICE_EVENT_LISTEN_STARTED: emit listenStarted(id); break;
    case P1_VOICE_EVENT_LISTEN_PARTIAL: emit listenPartial(id, text); break;
    case P1_VOICE_EVENT_LISTEN_RESULT: endListen(); emit listenResult(id, text, confidence); break;
    case P1_VOICE_EVENT_LISTEN_CANCELLED: endListen(); emit listenCancelled(id); break;
    case P1_VOICE_EVENT_ERROR: endListen(); emit errorOccurred(id, code, text.isEmpty() ? errorText(code) : text); break;
    default: break;
    }
}

}  // namespace p1::app
