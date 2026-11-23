#include "app/voice/VoiceNavigationBridge.h"

#include "app/voice/VoiceService.h"
#include "contracts/NavigationController.h"

#include <QRegularExpression>
#include <QStringList>

#include <cmath>

namespace p1::app {

using namespace p1::contracts;

namespace {

// Whisper 偶尔输出繁体；只转换指令中出现的关键字。
QString toSimplified(QString s) {
    static const std::pair<const char*, const char*> map[] = {
        {"導", "导"}, {"開", "开"}, {"啟", "启"}, {"繼", "继"}, {"續", "续"}, {"規", "规"}, {"劃", "划"},
        {"縮", "缩"}, {"語", "语"}, {"音", "音"}, {"關", "关"}, {"閉", "闭"}, {"還", "还"}, {"遠", "远"},
        {"線", "线"}, {"圖", "图"}, {"暫", "暂"}, {"單", "单"}, {"結", "结"}, {"東", "东"}, {"這", "这"},
        {"裡", "里"}, {"際", "际"}, {"幾", "几"}, {"點", "点"}, {"達", "达"}, {"報", "报"}, {"從", "从"},
        {"啓", "启"}, {"說", "说"}, {"聽", "听"}, {"嗎", "吗"}, {"麼", "么"}, {"漢", "汉"}, {"學", "学"}};
    for (const auto& [t, simp] : map) s.replace(QString::fromUtf8(t), QString::fromUtf8(simp));
    return s;
}

QString normalizeHeard(const QString& text) {
    QString s = toSimplified(text);
    s.remove(QRegularExpression(QStringLiteral("[\\s，。！？、,.!?；;：:“”\"']")));
    for (const char* prefix : {"请帮我", "帮我", "请你", "请", "麻烦", "我要", "我想"}) {
        const QString p = QString::fromUtf8(prefix);
        if (s.startsWith(p)) { s = s.mid(p.size()); break; }
    }
    return s;
}

bool containsAny(const QString& s, std::initializer_list<const char*> words) {
    for (const char* w : words) if (s.contains(QString::fromUtf8(w))) return true;
    return false;
}

}  // namespace

VoiceCommand parseVoiceCommand(const QString& text) {
    VoiceCommand c;
    c.heard = text;
    const QString s = normalizeHeard(text);
    if (s.isEmpty()) return c;
    // 顺序从具体到一般：“关闭语音播报”不能被当成“停止导航”，“继续导航”不能被当成“开始导航”。
    static const QRegularExpression navigateTo(QStringLiteral("^(?:导航|带我|开车)?(?:到|去|前往)(.+)$"));
    if (containsAny(s, {"关闭语音", "关掉语音", "关闭播报", "静音", "别说话"})) c.kind = VoiceCommandKind::VoiceGuidanceOff;
    else if (containsAny(s, {"打开语音", "开启语音", "打开播报", "开启播报"})) c.kind = VoiceCommandKind::VoiceGuidanceOn;
    else if (containsAny(s, {"重新规划", "重新计算", "重新导航", "换条路", "换一条路", "重算"})) c.kind = VoiceCommandKind::Reroute;
    else if (containsAny(s, {"暂停"})) c.kind = VoiceCommandKind::PauseNavigation;
    else if (containsAny(s, {"继续导航", "恢复导航", "继续"})) c.kind = VoiceCommandKind::ResumeNavigation;
    else if (containsAny(s, {"停止导航", "结束导航", "退出导航", "取消导航", "停止", "结束"})) c.kind = VoiceCommandKind::StopNavigation;
    else if (containsAny(s, {"开始导航", "出发", "启动导航", "开始"})) c.kind = VoiceCommandKind::StartNavigation;
    else if (containsAny(s, {"还有多远", "多远", "还有多久", "多久", "剩余", "几点到", "什么时候到", "到达时间"})) c.kind = VoiceCommandKind::AskRemaining;
    else if (containsAny(s, {"放大"})) c.kind = VoiceCommandKind::ZoomIn;
    else if (containsAny(s, {"缩小"})) c.kind = VoiceCommandKind::ZoomOut;
    else if (const auto m = navigateTo.match(s); m.hasMatch() && !m.captured(1).isEmpty()) {
        c.kind = VoiceCommandKind::NavigateTo;
        c.argument = m.captured(1);
    }
    return c;
}

QString voiceCommandName(VoiceCommandKind k) {
    switch (k) {
    case VoiceCommandKind::None: return QStringLiteral("未识别的指令");
    case VoiceCommandKind::StartNavigation: return QStringLiteral("开始导航");
    case VoiceCommandKind::StopNavigation: return QStringLiteral("停止导航");
    case VoiceCommandKind::PauseNavigation: return QStringLiteral("暂停导航");
    case VoiceCommandKind::ResumeNavigation: return QStringLiteral("继续导航");
    case VoiceCommandKind::Reroute: return QStringLiteral("重新规划路线");
    case VoiceCommandKind::AskRemaining: return QStringLiteral("查询剩余距离");
    case VoiceCommandKind::ZoomIn: return QStringLiteral("放大地图");
    case VoiceCommandKind::ZoomOut: return QStringLiteral("缩小地图");
    case VoiceCommandKind::NavigateTo: return QStringLiteral("导航到目的地");
    case VoiceCommandKind::VoiceGuidanceOn: return QStringLiteral("打开语音播报");
    case VoiceCommandKind::VoiceGuidanceOff: return QStringLiteral("关闭语音播报");
    }
    return {};
}

QString distanceSpeech(double m) {
    if (!std::isfinite(m) || m < 0) return {};
    if (m < 20) return QStringLiteral("即将");
    if (m < 1000) {
        const int step = m < 200 ? 10 : 50;  // 播报取整，避免“前方 287 米”
        const int r = static_cast<int>(std::lround(m / step) * step);
        return QStringLiteral("前方%1米").arg(r);
    }
    const double km = std::round(m / 100.0) / 10.0;
    return QStringLiteral("前方%1公里").arg(QString::number(km, 'f', km == std::floor(km) ? 0 : 1));
}

QString guidanceSpeech(const GuidanceInstruction& g) {
    if (!g.text.trimmed().isEmpty()) {
        // 引导模块已给出完整文本时，只在开头补距离（文本本身已含距离则原样播报）。
        const QString t = g.text.trimmed();
        if (t.contains(QStringLiteral("米")) || t.contains(QStringLiteral("公里")) || g.kind == GuidanceKind::Depart ||
            g.kind == GuidanceKind::ArriveDestination)
            return t;
        const QString d = distanceSpeech(g.distanceToManeuverM);
        return d.isEmpty() ? t : d + t;
    }
    QString action;
    switch (g.kind) {
    case GuidanceKind::Depart: return QStringLiteral("开始导航，请沿当前道路行驶");
    case GuidanceKind::ArriveDestination: return QStringLiteral("您已到达目的地，本次导航结束");
    case GuidanceKind::ContinueStraight: action = QStringLiteral("继续直行"); break;
    case GuidanceKind::TurnLeft: action = QStringLiteral("左转"); break;
    case GuidanceKind::TurnRight: action = QStringLiteral("右转"); break;
    case GuidanceKind::SlightLeft: action = QStringLiteral("稍向左转"); break;
    case GuidanceKind::SlightRight: action = QStringLiteral("稍向右转"); break;
    case GuidanceKind::SharpLeft: action = QStringLiteral("向左急转"); break;
    case GuidanceKind::SharpRight: action = QStringLiteral("向右急转"); break;
    case GuidanceKind::UTurn: action = QStringLiteral("掉头"); break;
    case GuidanceKind::KeepLeft: action = QStringLiteral("靠左行驶"); break;
    case GuidanceKind::KeepRight: action = QStringLiteral("靠右行驶"); break;
    case GuidanceKind::RoundaboutEnter: action = QStringLiteral("进入环岛"); break;
    case GuidanceKind::RoundaboutExit: action = QStringLiteral("驶出环岛"); break;
    case GuidanceKind::Merge: action = QStringLiteral("汇入主路"); break;
    case GuidanceKind::Unknown: return {};
    }
    return distanceSpeech(g.distanceToManeuverM) + action;
}

VoiceNavigationBridge::VoiceNavigationBridge(VoiceService* voice, QObject* parent) : QObject(parent), voice_(voice) {
    qRegisterMetaType<p1::app::VoiceCommand>("p1::app::VoiceCommand");
    connect(voice_, &VoiceService::listenResult, this,
            [this](std::uint64_t, const QString& text, float) { handleRecognized(text); });
}

void VoiceNavigationBridge::attach(NavigationController* c) {
    detach();
    controller_ = c;
    if (!c) return;
    connect(c, &NavigationController::guidanceUpdated, this, &VoiceNavigationBridge::onGuidance);
    connect(c, &NavigationController::navigationEventOccurred, this, &VoiceNavigationBridge::onEvent);
}

void VoiceNavigationBridge::detach() {
    if (controller_) disconnect(controller_, nullptr, this, nullptr);
    controller_ = nullptr;
    lastGuidanceIndex_.reset();
}

void VoiceNavigationBridge::setGuidanceEnabled(bool enabled) {
    if (guidanceEnabled_ == enabled) return;
    guidanceEnabled_ = enabled;
    if (!enabled) voice_->cancel(0);
}

void VoiceNavigationBridge::say(const QString& text, bool urgent) {
    if (text.isEmpty() || !guidanceEnabled_) return;
    if (voice_->speak(text, urgent)) {
        ++spoken_;
        emit announcement(text);
    }
}

void VoiceNavigationBridge::onGuidance(const GuidanceInstruction& g) {
    if (controller_ && controller_->navigationId() != lastNavigationId_) {
        lastNavigationId_ = controller_->navigationId();
        lastGuidanceIndex_.reset();
    }
    if (lastGuidanceIndex_ && *lastGuidanceIndex_ == g.index) return;  // 同一条引导只播一次
    if (controller_ && controller_->paused()) return;                    // 暂停时不推进播报
    lastGuidanceIndex_ = g.index;
    say(guidanceSpeech(g), g.kind == GuidanceKind::ArriveDestination);
}

void VoiceNavigationBridge::onEvent(const NavigationEvent& e) {
    switch (e.kind) {
    case NavigationEventKind::OffRouteDetected: say(QStringLiteral("您已偏离路线，正在重新规划"), true); break;
    case NavigationEventKind::RouteUpdated:
        lastGuidanceIndex_.reset();  // 重算后新路线自身的序号重新开始
        say(QStringLiteral("已为您重新规划路线"), false);
        break;
    case NavigationEventKind::Arrived: say(QStringLiteral("您已到达目的地，本次导航结束"), true); break;
    case NavigationEventKind::NavigationAborted: say(QStringLiteral("导航已中止，") + e.reason, true); break;
    case NavigationEventKind::NavigationPaused: say(QStringLiteral("导航已暂停"), true); break;
    case NavigationEventKind::NavigationResumed: say(QStringLiteral("继续导航"), false); break;
    case NavigationEventKind::NavigationStopped:
        lastGuidanceIndex_.reset();
        voice_->cancel(0);
        break;
    default: break;
    }
}

void VoiceNavigationBridge::answerRemaining() {
    if (!controller_) { say(QStringLiteral("当前没有进行中的导航"), true); return; }
    const NavigationSnapshot s = controller_->snapshot();
    if (!s.remainingDistanceM) { say(QStringLiteral("当前没有进行中的导航"), true); return; }
    const double m = *s.remainingDistanceM;
    QString text = m >= 1000 ? QStringLiteral("距离目的地还有%1公里").arg(QString::number(m / 1000.0, 'f', 1))
                             : QStringLiteral("距离目的地还有%1米").arg(static_cast<int>(std::lround(m / 10) * 10));
    if (s.remainingDurationSec) text += QStringLiteral("，预计%1分钟").arg(std::max(1, int(std::lround(*s.remainingDurationSec / 60))));
    // 询问的回答不受“关闭语音播报”限制。
    if (voice_->speak(text, true)) { ++spoken_; emit announcement(text); }
}

VoiceCommand VoiceNavigationBridge::handleRecognized(const QString& text) {
    const VoiceCommand c = parseVoiceCommand(text);
    emit commandRecognized(c);
    switch (c.kind) {
    case VoiceCommandKind::PauseNavigation:
        if (!controller_ || !controller_->pause()) say(QStringLiteral("当前无法暂停导航"), true);
        break;
    case VoiceCommandKind::ResumeNavigation:
        if (!controller_ || !controller_->resume()) say(QStringLiteral("当前没有暂停的导航"), true);
        break;
    case VoiceCommandKind::StopNavigation:
        if (controller_ && controller_->status() != NavigationStatus::Idle) {
            controller_->stop();
            say(QStringLiteral("导航已结束"), true);
        } else {
            say(QStringLiteral("当前没有进行中的导航"), true);
        }
        break;
    case VoiceCommandKind::StartNavigation: emit startNavigationRequested(); break;
    case VoiceCommandKind::Reroute: emit rerouteRequested(); break;
    case VoiceCommandKind::NavigateTo: emit navigateToRequested(c.argument); break;
    case VoiceCommandKind::ZoomIn: emit zoomRequested(true); break;
    case VoiceCommandKind::ZoomOut: emit zoomRequested(false); break;
    case VoiceCommandKind::AskRemaining: answerRemaining(); break;
    case VoiceCommandKind::VoiceGuidanceOn:
        setGuidanceEnabled(true);
        say(QStringLiteral("语音播报已打开"), false);
        break;
    case VoiceCommandKind::VoiceGuidanceOff:
        if (voice_->speak(QStringLiteral("语音播报已关闭"), true)) ++spoken_;
        guidanceEnabled_ = false;
        break;
    case VoiceCommandKind::None: break;
    }
    return c;
}

}  // namespace p1::app
