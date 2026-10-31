#include "app/ImportTask.h"

#include "app/ImportParser.h"

#include <QMetaType>

#include <utility>

namespace p1::app {

ImportTask::ImportTask(std::unique_ptr<ImportParser> parser, QString filePath, QObject* parent)
    : QThread(parent), parser_(std::move(parser)), filePath_(std::move(filePath)) {
    // Required so a queued signal can carry the value across the thread boundary.
    qRegisterMetaType<ImportResult>("p1::app::ImportResult");
    setObjectName(QStringLiteral("P1ImportTask"));
}

ImportTask::~ImportTask() {
    if (isRunning()) {
        cancel();
        wait();
    }
}

void ImportTask::cancel() {
    cancelled_.store(true, std::memory_order_relaxed);
}

void ImportTask::run() {
    ImportResult result;
    result.filePath = filePath_;
    if (!parser_) {
        result.ok = false;
        result.error = QStringLiteral("没有可用于该文件的解析器");
        emit completed(result);
        return;
    }

    result = parser_->parse(
        filePath_,
        [this](int percent, const QString& stage) { emit progressChanged(percent, stage); },
        [this](const QString& message) { emit logMessage(message); },
        [this]() { return cancelled_.load(std::memory_order_relaxed); });

    emit completed(result);
}

} // namespace p1::app
