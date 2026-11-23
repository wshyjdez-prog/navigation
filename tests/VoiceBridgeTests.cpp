// 1 号：语音服务 / 导航桥接测试。
// 使用真实 LinuxVoiceAdapter（经 C ABI，Piper + Vosk）：TTS 写 WAV、ASR 读由 Piper 合成的 WAV，不需要声卡；
// 导航核心用只实现公共契约的 FakeController，验证“识别结果 → FFI 回调 → 主线程 → 核心命令”。
#include "app/voice/VoiceNavigationBridge.h"
#include "app/voice/VoiceService.h"
#include "contracts/NavigationController.h"
#include "contracts/VoiceFfi.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace p1::app;
using namespace p1::contracts;

namespace {

class FakeController final : public NavigationController {
public:
    NavigationStatus status_ = NavigationStatus::Navigating;
    bool paused_ = false;
    std::uint64_t navId = 1;
    int pauses = 0, resumes = 0, stops = 0;
    std::optional<double> remaining;
    NavigationSnapshot snapshot() const override {
        NavigationSnapshot s;
        s.status = status_;
        s.paused = paused_;
        s.navigationId = navId;
        s.remainingDistanceM = remaining;
        if (remaining) s.remainingDurationSec = *remaining / 10.0;
        return s;
    }
    NavigationStatus status() const override { return status_; }
    bool paused() const override { return paused_; }
    std::uint64_t navigationId() const override { return navId; }
    std::uint64_t currentRequestId() const override { return 0; }
    std::uint64_t start(const RoutePlanRequest&) override { return 0; }
    bool pause() override {
        if (paused_ || status_ != NavigationStatus::Navigating) return false;
        paused_ = true;
        ++pauses;
        return true;
    }
    bool resume() override {
        if (!paused_) return false;
        paused_ = false;
        ++resumes;
        return true;
    }
    void stop() override { status_ = NavigationStatus::Idle; paused_ = false; ++stops; }
    void locationUpdated(const PositionSample&) override {}
    std::uint64_t requestReroute(const RerouteRequest&) override { return 0; }
    void routeResultReady(const RoutePlanResult&) override {}
    void routeFailed(std::uint64_t, NavigationErrorCode, const QString&) override {}
    void arrived(std::uint64_t) override {}
    void offRoute(std::uint64_t) override {}
    void emitGuidance(GuidanceKind k, double d, std::uint32_t index) {
        GuidanceInstruction g;
        g.kind = k;
        g.distanceToManeuverM = d;
        g.index = index;
        emit guidanceUpdated(g);
    }
    void emitEvent(NavigationEventKind k) {
        NavigationEvent e;
        e.kind = k;
        emit navigationEventOccurred(e);
    }
};

}  // namespace

class VoiceBridgeTests final : public QObject {
    Q_OBJECT
private slots:
    void parsesCommands_data() {
        QTest::addColumn<QString>("heard");
        QTest::addColumn<int>("kind");
        QTest::addColumn<QString>("argument");
        const auto row = [](const char* name, const char* heard, VoiceCommandKind k, const char* arg = "") {
            QTest::newRow(name) << QString::fromUtf8(heard) << int(k) << QString::fromUtf8(arg);
        };
        row("start", "开始导航", VoiceCommandKind::StartNavigation);
        row("start vosk spaces", "开始 导航", VoiceCommandKind::StartNavigation);
        row("start whisper punct", "开始导航。", VoiceCommandKind::StartNavigation);
        row("start traditional", "開始導航", VoiceCommandKind::StartNavigation);
        row("stop", "停止导航", VoiceCommandKind::StopNavigation);
        row("stop polite", "请帮我结束导航", VoiceCommandKind::StopNavigation);
        row("pause", "暂停导航", VoiceCommandKind::PauseNavigation);
        row("pause traditional", "暫停導航", VoiceCommandKind::PauseNavigation);
        row("resume", "继续导航", VoiceCommandKind::ResumeNavigation);
        row("resume traditional", "繼續導航", VoiceCommandKind::ResumeNavigation);
        row("reroute", "重新规划路线", VoiceCommandKind::Reroute);
        row("reroute other", "换一条路", VoiceCommandKind::Reroute);
        row("remaining", "还有多远", VoiceCommandKind::AskRemaining);
        row("eta", "现在几点到达", VoiceCommandKind::AskRemaining);
        row("zoom in", "放大地图", VoiceCommandKind::ZoomIn);
        row("zoom out", "缩小 地图", VoiceCommandKind::ZoomOut);
        row("voice off", "关闭语音播报", VoiceCommandKind::VoiceGuidanceOff);
        row("voice on", "打开语音播报", VoiceCommandKind::VoiceGuidanceOn);
        row("navigate to", "导航到武汉大学", VoiceCommandKind::NavigateTo, "武汉大学");
        row("go to", "我想去黄鹤楼", VoiceCommandKind::NavigateTo, "黄鹤楼");
        row("noise", "今天天气不错", VoiceCommandKind::None);
        row("empty", "", VoiceCommandKind::None);
    }
    void parsesCommands() {
        QFETCH(QString, heard);
        QFETCH(int, kind);
        QFETCH(QString, argument);
        const VoiceCommand c = parseVoiceCommand(heard);
        QCOMPARE(int(c.kind), kind);
        QCOMPARE(c.argument, argument);
    }

    void formatsGuidance() {
        QCOMPARE(distanceSpeech(287), QStringLiteral("前方300米"));
        QCOMPARE(distanceSpeech(52), QStringLiteral("前方50米"));
        QCOMPARE(distanceSpeech(1520), QStringLiteral("前方1.5公里"));
        QCOMPARE(distanceSpeech(2000), QStringLiteral("前方2公里"));
        QCOMPARE(distanceSpeech(5), QStringLiteral("即将"));
        GuidanceInstruction g;
        g.kind = GuidanceKind::TurnRight;
        g.distanceToManeuverM = 300;
        QCOMPARE(guidanceSpeech(g), QStringLiteral("前方300米右转"));
        g.text = QStringLiteral("右转进入中山大道");
        QCOMPARE(guidanceSpeech(g), QStringLiteral("前方300米右转进入中山大道"));
        g.kind = GuidanceKind::ArriveDestination;
        g.text.clear();
        QCOMPARE(guidanceSpeech(g), QStringLiteral("您已到达目的地，本次导航结束"));
    }

    // 引导 → 播报：同一 index 只播一次，暂停/关闭播报时不播，偏航打断。
    void speaksGuidanceOncePerIndex() {
        QTemporaryDir dir;
        VoiceService voice;
        VoiceOptions o;
        o.ttsBackend = "piper";
        o.asrBackend = "none";
        o.outputWavPath = dir.filePath("guidance.wav");
        voice.setOptions(o);
        VoiceNavigationBridge bridge(&voice);
        FakeController nav;
        bridge.attach(&nav);
        QSignalSpy said(&bridge, &VoiceNavigationBridge::announcement);
        QSignalSpy finished(&voice, &VoiceService::speakFinished);
        nav.emitGuidance(GuidanceKind::TurnRight, 300, 0);
        nav.emitGuidance(GuidanceKind::TurnRight, 280, 0);  // 同一 index：不重复
        QCOMPARE(said.count(), 1);
        QCOMPARE(said.at(0).at(0).toString(), QStringLiteral("前方300米右转"));
        nav.emitGuidance(GuidanceKind::TurnLeft, 120, 1);
        QCOMPARE(said.count(), 2);
        nav.paused_ = true;
        nav.emitGuidance(GuidanceKind::KeepLeft, 80, 2);  // 暂停：不推进
        QCOMPARE(said.count(), 2);
        nav.paused_ = false;
        nav.emitEvent(NavigationEventKind::OffRouteDetected);
        QCOMPARE(said.count(), 3);
        QCOMPARE(said.at(2).at(0).toString(), QStringLiteral("您已偏离路线，正在重新规划"));
        nav.emitEvent(NavigationEventKind::RouteUpdated);  // 新路线：序号重新开始
        nav.emitGuidance(GuidanceKind::TurnRight, 300, 0);
        QCOMPARE(said.count(), 5);
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 20000);  // 偏航提示打断了之前的播报，之后的仍正常完成
        QVERIFY(voice.ttsReady());
        bridge.setGuidanceEnabled(false);  // 关闭播报同时取消正在播与排队的内容
        nav.emitGuidance(GuidanceKind::TurnLeft, 50, 1);
        QCOMPARE(said.count(), 5);
    }

    // FFI 回调 → 主线程 → 核心层命令：用 Piper 合成指令语音，经 Vosk 识别后驱动控制器。
    void recognizedSpeechDrivesController() {
        const char* tts = "piper";
        QTemporaryDir dir;
        const auto synth = [&](const QString& text, const QString& name) {
            VoiceService s;
            VoiceOptions o;
            o.ttsBackend = tts;
            o.asrBackend = "none";
            o.outputWavPath = dir.filePath(name);
            s.setOptions(o);
            QSignalSpy done(&s, &VoiceService::speakFinished);
            s.speak(text);
            return done.wait(60000) ? dir.filePath(name) : QString();
        };
        const QString pauseWav = synth(QStringLiteral("暂停导航"), "pause.wav");
        const QString stopWav = synth(QStringLiteral("停止导航"), "stop.wav");
        QVERIFY(!pauseWav.isEmpty() && !stopWav.isEmpty());

        FakeController nav;
        // 返回识别出的指令；监听状态异常时返回 kind=None 且 heard 写入原因。
        const auto recognize = [&](const QString& wav) {
            VoiceService voice;
            VoiceOptions o;
            o.ttsBackend = "piper";
            o.asrBackend = "vosk";
            o.inputWavPath = wav;
            o.outputWavPath = dir.filePath("reply.wav");
            voice.setOptions(o);
            VoiceNavigationBridge bridge(&voice);
            bridge.attach(&nav);
            QSignalSpy cmd(&bridge, &VoiceNavigationBridge::commandRecognized);
            VoiceCommand none;
            if (voice.listen(15000) == 0 || !voice.listening()) { none.heard = "listen rejected"; return none; }
            if (!cmd.wait(60000)) { none.heard = "no command"; return none; }
            if (voice.listening()) { none.heard = "still listening"; return none; }
            return qvariant_cast<VoiceCommand>(cmd.at(0).at(0));
        };
        const VoiceCommand p = recognize(pauseWav);
        qInfo() << "heard:" << p.heard;
        QCOMPARE(int(p.kind), int(VoiceCommandKind::PauseNavigation));
        QCOMPARE(nav.pauses, 1);
        QVERIFY(nav.paused_);
        const VoiceCommand s = recognize(stopWav);
        qInfo() << "heard:" << s.heard;
        QCOMPARE(int(s.kind), int(VoiceCommandKind::StopNavigation));
        QCOMPARE(nav.stops, 1);
        QCOMPARE(nav.status_, NavigationStatus::Idle);
    }

    void answersRemainingDistance() {
        QTemporaryDir dir;
        VoiceService voice;
        VoiceOptions o;
        o.ttsBackend = "piper";
        o.asrBackend = "none";
        o.outputWavPath = dir.filePath("r.wav");
        voice.setOptions(o);
        VoiceNavigationBridge bridge(&voice);
        FakeController nav;
        nav.remaining = 2340;
        bridge.attach(&nav);
        QSignalSpy said(&bridge, &VoiceNavigationBridge::announcement);
        bridge.handleRecognized(QStringLiteral("还有多远"));
        QCOMPARE(said.count(), 1);
        QCOMPARE(said.at(0).at(0).toString(), QStringLiteral("距离目的地还有2.3公里，预计4分钟"));
        bridge.handleRecognized(QStringLiteral("关闭语音播报"));
        QVERIFY(!bridge.guidanceEnabled());
        nav.emitGuidance(GuidanceKind::TurnLeft, 100, 7);
        QCOMPARE(said.count(), 1);  // 关闭后不再播报引导
    }

    // 服务生命周期：播报中 stop() 立即返回，之后不再投递旧实例的信号；restart 后可继续使用。
    void stopDuringSpeechDropsLateSignals() {
        VoiceService voice;
        VoiceOptions o;
        o.ttsBackend = "piper";
        o.asrBackend = "none";
        voice.setOptions(o);  // 实际播放到默认输出（auto_null），按实时推进
        QSignalSpy started(&voice, &VoiceService::speakStarted);
        QSignalSpy any(&voice, &VoiceService::speakCancelled);
        QVERIFY(voice.speak(QStringLiteral("这是一段较长的播报内容，用于验证在播报过程中停止语音服务")) != 0);
        QVERIFY(started.wait(20000));
        QElapsedTimer t;
        t.start();
        voice.stop();
        QVERIFY(t.elapsed() < 3000);
        QVERIFY(!voice.running());
        QTest::qWait(300);
        QCOMPARE(any.count(), 0);  // 旧实例的取消事件属于上一代，已被丢弃
        QVERIFY(voice.restart(o));
        QSignalSpy finished(&voice, &VoiceService::speakFinished);
        voice.speak(QStringLiteral("恢复"));
        QVERIFY(finished.wait(20000));
    }

    void defaultsToSelectedBackends() {
        QCOMPARE(VoiceService::compiledBackends(), QStringLiteral("tts=piper,;asr=vosk,"));
    }

    void reportsMicrophoneState() {
        QString name;
        const int r = VoiceService::checkMicrophone(&name);
        QVERIFY(r == P1_VOICE_OK || r == P1_VOICE_ERR_DEVICE || r == P1_VOICE_ERR_PERMISSION_DENIED);
        QVERIFY(!VoiceService::errorText(r).isEmpty());
    }
};

QTEST_GUILESS_MAIN(VoiceBridgeTests)
#include "VoiceBridgeTests.moc"
