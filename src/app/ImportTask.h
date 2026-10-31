#pragma once

#include "app/ImportTypes.h"

#include <QThread>

#include <atomic>
#include <memory>

namespace p1::app {

class ImportParser;

// Runs one import in a background thread. Progress, log lines and the final
// result are delivered to the GUI thread through queued signals; the parser
// never touches a widget. Mirrors the osmscout pattern of a job that reports
// progress and can be cancelled while it works.
class ImportTask final : public QThread {
    Q_OBJECT
public:
    ImportTask(std::unique_ptr<ImportParser> parser, QString filePath, QObject* parent = nullptr);
    ~ImportTask() override;

    void cancel();
    QString filePath() const { return filePath_; }

signals:
    void progressChanged(int percent, const QString& stage);
    void logMessage(const QString& message);
    void completed(const ImportResult& result);

protected:
    void run() override;

private:
    std::unique_ptr<ImportParser> parser_;
    QString filePath_;
    std::atomic_bool cancelled_{false};
};

} // namespace p1::app
