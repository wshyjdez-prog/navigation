#pragma once
// 1 号：Qt 侧的语音服务。经 VoiceFfi.h（C ABI）持有一个 LinuxVoiceAdapter 实例，
// 把适配器分发线程上的回调转成主线程信号（Qt::QueuedConnection），UI 与导航核心只连信号。
//
// 生命周期：start() 创建适配器（后台加载引擎，不阻塞 UI）；restart() 先销毁旧实例；
// 析构/stop() 在主线程调用 p1_voice_destroy，返回后不再有回调；已排队但未投递的
// 信号随 QObject 销毁被 Qt 丢弃。不得在本对象的槽里同步销毁本对象（用 deleteLater）。
#include <QObject>
#include <QString>

#include <cstdint>

struct P1VoiceAdapter;
struct P1VoiceEvent;

namespace p1::app {

struct VoiceOptions {
    QString ttsBackend;      // 空 = 库默认
    QString asrBackend;
    QString resourceDir;     // 空 = 按安装布局查找
    QString outputWavPath;   // 测试：TTS 写 WAV
    QString inputWavPath;    // 测试：ASR 读 WAV
    bool inputRealtime = false;
    int speechRate = 0;
};

class VoiceService final : public QObject {
    Q_OBJECT
public:
    explicit VoiceService(QObject* parent = nullptr);
    ~VoiceService() override;

    // 记录选项但不启动；speak()/listen() 首次调用时按需启动。
    void setOptions(const VoiceOptions& options) { options_ = options; }
    bool start(const VoiceOptions& options);
    bool restart(const VoiceOptions& options) { stop(); return start(options); }
    void stop();
    bool running() const { return adapter_ != nullptr; }
    const VoiceOptions& options() const { return options_; }
    bool ttsReady() const { return ttsReady_; }
    bool asrReady() const { return asrReady_; }
    bool listening() const { return listenId_ != 0; }

    // 返回请求 ID；0 表示被拒绝（原因经 errorOccurred 报告）。
    std::uint64_t speak(const QString& text, bool interrupt = false);
    std::uint64_t listen(int timeoutMs = 8000);
    void stopListening();
    void cancel(std::uint64_t requestId = 0);

    static QString compiledBackends();
    static QString errorText(int code);
    // 同步、约 2 秒超时；返回 P1VoiceError，name 写入默认输入源。
    static int checkMicrophone(QString* name);

signals:
    void serviceStatus(const QString& subsystem, bool ok, const QString& detail);
    void speakStarted(std::uint64_t id);
    void speakFinished(std::uint64_t id);
    void speakCancelled(std::uint64_t id);
    void listenStarted(std::uint64_t id);
    void listenPartial(std::uint64_t id, const QString& text);
    void listenResult(std::uint64_t id, const QString& text, float confidence);
    void listenCancelled(std::uint64_t id);
    void errorOccurred(std::uint64_t id, int code, const QString& message);
    void listeningChanged(bool listening);

private:
    struct CallbackContext;
    static void onEvent(const P1VoiceEvent* event, void* user);
    void deliver(int type, std::uint64_t id, int code, const QString& text, float confidence);

    P1VoiceAdapter* adapter_ = nullptr;
    CallbackContext* context_ = nullptr;
    VoiceOptions options_;
    bool ttsReady_ = false;
    bool asrReady_ = false;
    std::uint64_t listenId_ = 0;
    std::uint64_t generation_ = 0;  // 丢弃旧实例残留的排队信号
};

}  // namespace p1::app
