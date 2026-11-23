#pragma once
// 1 号：语音助手页面（路由 "voice"）。展示 LinuxVoiceAdapter 状态，提供播报（speak）、
// 语音指令（listen）、麦克风检查与事件日志。
#include <QWidget>

namespace p1::ui { class Theme; }

namespace p1::app {

class VoiceService;
class VoiceNavigationBridge;

QWidget* createVoicePage(ui::Theme& theme, VoiceService* voice, VoiceNavigationBridge* bridge);

}  // namespace p1::app
