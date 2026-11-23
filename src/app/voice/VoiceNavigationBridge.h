#pragma once
// 1 号：语音与导航核心的桥接（第 3 次作业：语音回调经 FFI 传回核心层）。
//  - 播报：订阅 4 号 NavigationController 契约的引导/事件信号，生成中文播报文本调用 speak()；
//    同一 guidance index 不重复播报，偏航/到达等紧急提示打断当前播报。
//  - 控制：ASR 结果（FFI 回调 → VoiceService 主线程信号）解析为语音指令，
//    暂停/继续/停止直接调用 NavigationController 契约命令；需要起终点等业务数据的指令
//    （开始导航、重新规划、导航到某地、缩放）以信号交给应用层处理，不越权构造请求。
// 只依赖 include/contracts 中的公共契约，不读取 4 号状态机内部状态。
#include "contracts/NavigationTypes.h"

#include <QObject>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <optional>

namespace p1::contracts { class NavigationController; }

namespace p1::app {

class VoiceService;

enum class VoiceCommandKind {
    None,
    StartNavigation,
    StopNavigation,
    PauseNavigation,
    ResumeNavigation,
    Reroute,
    AskRemaining,
    ZoomIn,
    ZoomOut,
    NavigateTo,
    VoiceGuidanceOn,
    VoiceGuidanceOff
};

struct VoiceCommand {
    VoiceCommandKind kind = VoiceCommandKind::None;
    QString argument;  // NavigateTo 的目的地名称
    QString heard;     // 原始识别文本
};

// 识别文本 → 指令。容忍 ASR 的分词空格、句末标点、繁体输出与“请/帮我”等口语前缀。
VoiceCommand parseVoiceCommand(const QString& text);
QString voiceCommandName(VoiceCommandKind kind);
// 引导指令 → 播报文本（text 为空时按动作类型与距离生成）。
QString guidanceSpeech(const p1::contracts::GuidanceInstruction& instruction);
QString distanceSpeech(double metres);

class VoiceNavigationBridge final : public QObject {
    Q_OBJECT
public:
    explicit VoiceNavigationBridge(VoiceService* voice, QObject* parent = nullptr);
    void attach(p1::contracts::NavigationController* controller);
    void detach();
    bool guidanceEnabled() const { return guidanceEnabled_; }
    void setGuidanceEnabled(bool enabled);
    // 应用层的一次性提示（受“关闭语音播报”控制）。
    void announce(const QString& text, bool urgent) { say(text, urgent); }
    // 处理一条识别文本（也供界面手动输入调试）。
    VoiceCommand handleRecognized(const QString& text);
    int announcementsSpoken() const { return spoken_; }

signals:
    void commandRecognized(const p1::app::VoiceCommand& command);
    // 需要应用层提供业务数据的指令。
    void startNavigationRequested();
    void rerouteRequested();
    void navigateToRequested(const QString& destination);
    void zoomRequested(bool zoomIn);
    void announcement(const QString& text);

private:
    void onGuidance(const p1::contracts::GuidanceInstruction& instruction);
    void onEvent(const p1::contracts::NavigationEvent& event);
    void say(const QString& text, bool urgent);
    void answerRemaining();

    VoiceService* voice_;
    QPointer<p1::contracts::NavigationController> controller_;
    std::optional<std::uint32_t> lastGuidanceIndex_;
    std::uint64_t lastNavigationId_ = 0;
    bool guidanceEnabled_ = true;
    int spoken_ = 0;
};

}  // namespace p1::app

Q_DECLARE_METATYPE(p1::app::VoiceCommand)
