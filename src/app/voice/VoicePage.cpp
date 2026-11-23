#include "app/voice/VoicePage.h"

#include "app/voice/VoiceNavigationBridge.h"
#include "app/voice/VoiceService.h"
#include "contracts/VoiceFfi.h"
#include "ui/common/Components.h"
#include "ui/common/Theme.h"

#include <QDateTime>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QShowEvent>

namespace p1::app {

using namespace ui;

namespace {


class VoicePage final : public QWidget {
public:
    VoicePage(Theme& theme, VoiceService* voice, VoiceNavigationBridge* bridge)
        : voice_(voice), bridge_(bridge) {
        setObjectName("voicePage");
        auto* content = new QWidget;
        auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(6, 0, 6, 12);
        layout->setSpacing(24);

        // ── 服务状态 ──────────────────────────────────────────────
        auto* service = new Card(theme);
        service->body()->addWidget(label("语音服务", "sectionTitle"));
        service->body()->addWidget(label("本地离线中文语音：speak() 调用本地 TTS，listen() 调用本地 ASR，"
                                         "回调经 C ABI（FFI）返回核心层。", "secondary"));
        auto* form = new QFormLayout;
        // 后端按 DEC-20261123-1-voice-backend-01 选定：Piper 播报 + Vosk 识别。
        auto* engines = label("Piper（zh_CN-huayan）播报 · Vosk（普通话小模型）识别 · 全部离线", "body");
        engines->setObjectName("voiceEngines");
        form->addRow("语音引擎", engines);
        ttsState_ = label("未启动", "secondary");
        ttsState_->setObjectName("voiceTtsState");
        asrState_ = label("未启动", "secondary");
        asrState_->setObjectName("voiceAsrState");
        micState_ = label("未检查", "secondary");
        micState_->setObjectName("voiceMicState");
        micState_->setWordWrap(true);
        form->addRow("播报（TTS）", ttsState_);
        form->addRow("识别（ASR）", asrState_);
        form->addRow("麦克风", micState_);
        service->body()->addLayout(form);
        auto* serviceButtons = new QHBoxLayout;
        auto* restart = button("重启语音服务");
        restart->setObjectName("voiceRestart");
        auto* checkMic = button("检查麦克风");
        checkMic->setObjectName("voiceCheckMic");
        serviceButtons->addWidget(restart);
        serviceButtons->addWidget(checkMic);
        serviceButtons->addStretch();
        service->body()->addLayout(serviceButtons);
        layout->addWidget(service);

        // ── 播报 ──────────────────────────────────────────────────
        auto* speakCard = new Card(theme);
        speakCard->body()->addWidget(label("语音播报", "sectionTitle"));
        auto* speakRow = new QHBoxLayout;
        speakText_ = new QLineEdit("前方三百米右转，进入中山大道");
        speakText_->setObjectName("voiceSpeakText");
        speakText_->setAccessibleName("播报文本");
        auto* speak = button("播报", true);
        speak->setObjectName("voiceSpeak");
        auto* interrupt = button("打断并播报");
        interrupt->setObjectName("voiceInterrupt");
        auto* stop = button("停止");
        stop->setObjectName("voiceStopSpeak");
        speakRow->addWidget(speakText_, 1);
        speakRow->addWidget(speak);
        speakRow->addWidget(interrupt);
        speakRow->addWidget(stop);
        speakCard->body()->addLayout(speakRow);
        layout->addWidget(speakCard);

        // ── 语音指令 ──────────────────────────────────────────────
        auto* listenCard = new Card(theme);
        listenCard->body()->addWidget(label("语音指令", "sectionTitle"));
        listenCard->body()->addWidget(label("可说：开始导航 / 暂停导航 / 继续导航 / 停止导航 / 重新规划路线 / "
                                            "还有多远 / 放大地图 / 导航到某地 / 关闭语音播报", "secondary"));
        auto* listenRow = new QHBoxLayout;
        listen_ = button("开始说话", true);
        listen_->setObjectName("voiceListen");
        auto* fromFile = button("识别音频文件…");
        fromFile->setObjectName("voiceListenFile");
        listenRow->addWidget(listen_);
        listenRow->addWidget(fromFile);
        listenRow->addStretch();
        listenCard->body()->addLayout(listenRow);
        heard_ = label("—", "body");
        heard_->setObjectName("voiceHeard");
        command_ = label("—", "secondary");
        command_->setObjectName("voiceCommand");
        auto* resultForm = new QFormLayout;
        resultForm->addRow("识别结果", heard_);
        resultForm->addRow("解析指令", command_);
        listenCard->body()->addLayout(resultForm);
        auto* manualRow = new QHBoxLayout;
        manual_ = new QLineEdit;
        manual_->setObjectName("voiceManualText");
        manual_->setPlaceholderText("没有麦克风时，可输入识别文本测试指令解析");
        manual_->setAccessibleName("模拟识别文本");
        auto* manualGo = button("解析");
        manualGo->setObjectName("voiceManualParse");
        manualRow->addWidget(manual_, 1);
        manualRow->addWidget(manualGo);
        listenCard->body()->addLayout(manualRow);
        layout->addWidget(listenCard);

        // ── 事件日志 ──────────────────────────────────────────────
        auto* logCard = new Card(theme);
        logCard->body()->addWidget(label("回调事件", "sectionTitle"));
        log_ = new QListWidget;
        log_->setObjectName("voiceEventLog");
        log_->setMinimumHeight(180);
        logCard->body()->addWidget(log_);
        layout->addWidget(logCard);
        layout->addStretch();

        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->addWidget(scrollable(content));

        connect(restart, &QPushButton::clicked, this, [this] { restartService(); });
        connect(checkMic, &QPushButton::clicked, this, [this] { refreshMicrophone(); });
        connect(speak, &QPushButton::clicked, this, [this] { say(false); });
        connect(interrupt, &QPushButton::clicked, this, [this] { say(true); });
        connect(speakText_, &QLineEdit::returnPressed, this, [this] { say(false); });
        connect(stop, &QPushButton::clicked, this, [this] { ensureStarted(); voice_->cancel(0); });
        connect(listen_, &QPushButton::clicked, this, [this] { toggleListen(); });
        connect(fromFile, &QPushButton::clicked, this, [this] { recognizeFile(); });
        connect(manualGo, &QPushButton::clicked, this, [this] { bridge_->handleRecognized(manual_->text()); });
        connect(manual_, &QLineEdit::returnPressed, this, [this] { bridge_->handleRecognized(manual_->text()); });

        connect(voice_, &VoiceService::serviceStatus, this, [this](const QString& sub, bool ok, const QString& detail) {
            QLabel* target = sub == "tts" ? ttsState_ : asrState_;
            target->setText((ok ? QStringLiteral("● 就绪 · ") : QStringLiteral("● 不可用 · ")) + detail);
            target->setProperty("role", ok ? "body" : "error");
            addLog(QStringLiteral("SERVICE_%1  %2=%3").arg(ok ? "READY" : "ERROR", sub, detail));
        });
        connect(voice_, &VoiceService::speakStarted, this, [this](std::uint64_t id) { addLog(QStringLiteral("SPEAK_STARTED #%1").arg(id)); });
        connect(voice_, &VoiceService::speakFinished, this, [this](std::uint64_t id) { addLog(QStringLiteral("SPEAK_FINISHED #%1").arg(id)); });
        connect(voice_, &VoiceService::speakCancelled, this, [this](std::uint64_t id) { addLog(QStringLiteral("SPEAK_CANCELLED #%1").arg(id)); });
        connect(voice_, &VoiceService::listenStarted, this, [this](std::uint64_t id) {
            heard_->setText(QStringLiteral("正在聆听…"));
            addLog(QStringLiteral("LISTEN_STARTED #%1").arg(id));
        });
        connect(voice_, &VoiceService::listenPartial, this, [this](std::uint64_t, const QString& t) { heard_->setText(t + QStringLiteral(" …")); });
        connect(voice_, &VoiceService::listenResult, this, [this](std::uint64_t id, const QString& t, float c) {
            heard_->setText(t);
            addLog(QStringLiteral("LISTEN_RESULT #%1  %2%3").arg(id).arg(t, c >= 0 ? QStringLiteral("  (置信度 %1)").arg(c, 0, 'f', 2) : QString()));
        });
        connect(voice_, &VoiceService::listenCancelled, this, [this](std::uint64_t id) { addLog(QStringLiteral("LISTEN_CANCELLED #%1").arg(id)); });
        connect(voice_, &VoiceService::errorOccurred, this, [this](std::uint64_t id, int code, const QString& msg) {
            addLog(QStringLiteral("ERROR #%1  %2  %3").arg(id).arg(code).arg(msg));
            if (code == P1_VOICE_ERR_NO_SPEECH || code == P1_VOICE_ERR_TIMEOUT || code == P1_VOICE_ERR_DEVICE ||
                code == P1_VOICE_ERR_PERMISSION_DENIED)
                heard_->setText(msg);
        });
        connect(voice_, &VoiceService::listeningChanged, this, [this](bool on) {
            listen_->setText(on ? QStringLiteral("说完了") : QStringLiteral("开始说话"));
        });
        connect(bridge_, &VoiceNavigationBridge::commandRecognized, this, [this](const VoiceCommand& c) {
            QString t = voiceCommandName(c.kind);
            if (!c.argument.isEmpty()) t += QStringLiteral("：") + c.argument;
            command_->setText(t);
            addLog(QStringLiteral("COMMAND  %1  ← “%2”").arg(t, c.heard));
        });
    }

protected:
    void showEvent(QShowEvent* e) override {
        QWidget::showEvent(e);
        ensureStarted();
        if (!micChecked_) refreshMicrophone();
    }

private:
    VoiceOptions selectedOptions() const { return voice_->options(); }  // 空后端名 = 库默认（Piper + Vosk）
    void ensureStarted() {
        if (!voice_->running()) voice_->start(selectedOptions());
    }
    void restartService() {
        ttsState_->setText(QStringLiteral("正在加载…"));
        asrState_->setText(QStringLiteral("正在加载…"));
        voice_->restart(selectedOptions());
        addLog(QStringLiteral("重启语音服务"));
    }
    void say(bool interrupt) {
        ensureStarted();
        const QString t = speakText_->text().trimmed();
        if (t.isEmpty()) return;
        if (const auto id = voice_->speak(t, interrupt)) addLog(QStringLiteral("speak #%1  “%2”").arg(id).arg(t));
    }
    bool confirmMicrophone() {
        if (micConsent_) return true;
        // 应用内授权：首次使用麦克风时征得同意（本次会话有效）；系统级授权见 docs/voice-adapter.md。
        const auto r = QMessageBox::question(this, QStringLiteral("使用麦克风"),
            QStringLiteral("语音指令需要使用麦克风。音频只在本机离线识别，不保存、不上传。是否允许？"));
        micConsent_ = r == QMessageBox::Yes;
        return micConsent_;
    }
    void toggleListen() {
        ensureStarted();
        if (voice_->listening()) { voice_->stopListening(); return; }
        if (!confirmMicrophone()) { heard_->setText(QStringLiteral("未允许使用麦克风")); return; }
        if (const auto id = voice_->listen(8000)) addLog(QStringLiteral("listen #%1").arg(id));
    }
    void recognizeFile() {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择 16 位 PCM WAV"), {},
                                                          QStringLiteral("WAV 音频 (*.wav)"));
        if (path.isEmpty()) return;
        // 识别文件使用独立的临时服务实例，不打断主服务的播报。
        auto* fileVoice = new VoiceService(this);
        VoiceOptions o = selectedOptions();
        o.ttsBackend = QStringLiteral("none");
        o.inputWavPath = path;
        connect(fileVoice, &VoiceService::listenResult, this, [this, fileVoice](std::uint64_t, const QString& t, float) {
            heard_->setText(t);
            addLog(QStringLiteral("文件识别：%1").arg(t));
            bridge_->handleRecognized(t);
            fileVoice->deleteLater();
        });
        connect(fileVoice, &VoiceService::errorOccurred, this, [this, fileVoice](std::uint64_t id, int, const QString& msg) {
            if (id == 0) return;
            heard_->setText(msg);
            fileVoice->deleteLater();
        });
        fileVoice->start(o);
        fileVoice->listen(30000);
        heard_->setText(QStringLiteral("正在识别文件…"));
    }
    void refreshMicrophone() {
        micChecked_ = true;
        QString name;
        const int r = VoiceService::checkMicrophone(&name);
        micState_->setText(r == P1_VOICE_OK ? QStringLiteral("● 可用 · ") + name
                                            : QStringLiteral("● %1 · %2").arg(VoiceService::errorText(r), name));
    }
    void addLog(const QString& line) {
        log_->insertItem(0, QDateTime::currentDateTime().toString("HH:mm:ss.zzz  ") + line);
        while (log_->count() > 200) delete log_->takeItem(log_->count() - 1);
    }

    VoiceService* voice_;
    VoiceNavigationBridge* bridge_;
    QLabel *ttsState_, *asrState_, *micState_, *heard_, *command_;
    QLineEdit *speakText_, *manual_;
    QPushButton* listen_;
    QListWidget* log_;
    bool micChecked_ = false;
    bool micConsent_ = false;
};

}  // namespace

QWidget* createVoicePage(Theme& theme, VoiceService* voice, VoiceNavigationBridge* bridge) {
    return new VoicePage(theme, voice, bridge);
}

}  // namespace p1::app
