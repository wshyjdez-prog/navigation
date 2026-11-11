#pragma once

#include "app/ImportTypes.h"

#include <QWidget>
#include <QVector>

#include <memory>

class QLabel;
class QProgressBar;
class QPlainTextEdit;
class QPushButton;
class QResizeEvent;
class QListWidget;
class QTabWidget;

namespace p1::ui {
class Theme;
}

namespace p1::app {

class ImportParser;
class ImportTask;

// Data import page for the unified OSMX road network and standard OSM formats:
// file selection, background parsing with real progress, a running log and a
// success/failure summary. On success the OSMX dataset is published to
// p1::data::OsmxStore so algorithm and renderer modules share one instance.
// The page owns the worker thread and keeps working while the user navigates
// to another page, because the page itself stays alive in the window stack.
class ImportPage final : public QWidget {
    Q_OBJECT
public:
    explicit ImportPage(ui::Theme& theme, QWidget* parent = nullptr, const QString& cacheDirectory = {});
    ~ImportPage() override;

    // Public so tests and the window shell can preselect a file. An unreadable
    // path keeps the previous selection but disables "开始导入" until the user
    // chooses a readable file again.
    void selectFile(const QString& path);
    QString selectedFile() const { return selectedPath_; }
    bool isImporting() const { return importing_; }
    void startImport();
    // Re-opens the dataset selected in the local catalog: the dataset is parsed
    // from <cachePath>/data.osmx and published to OsmxStore again, so other
    // modules can read it after an application restart.
    void openSelectedDataset();

signals:
    void importFinished(const ImportResult& result);

protected:
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void chooseFile();
    void cancelImport();
    void onProgress(int percent, const QString& stage);
    void onLog(const QString& message);
    void onCompleted(const ImportResult& result);

private:
    void buildUi();
    void resetResultView();
    void setStatusBadge(const QString& text, const char* tone);
    void appendLog(const QString& message);
    void setBusy(bool busy);
    void refreshCatalog();
    void showSummary(const ImportResult& result);
    void updateOpenButtonState();
    // Starts one background import/load with the shared task lifecycle. The
    // page keeps ownership of the task until it reports completion.
    void startTask(std::unique_ptr<ImportParser> parser, const QString& filePath, const QString& initialStage);

    ui::Theme& theme_;
    QString selectedPath_;
    bool importing_ = false;
    QString cacheDirectory_;
    int catalogGeneration_ = 0;
    QVector<ImportResult> datasets_;
    QListWidget* datasetList_ = nullptr;
    QWidget* datasetTab_ = nullptr;
    QTabWidget* detailsTabs_ = nullptr;

    QLabel* fileName_ = nullptr;
    QLabel* fileInfo_ = nullptr;
    QLabel* statusBadge_ = nullptr;
    QLabel* versionValue_ = nullptr;
    QLabel* sourceValue_ = nullptr;
    QLabel* boundsValue_ = nullptr;
    QLabel* durationValue_ = nullptr;
    QLabel* nodesValue_ = nullptr;
    QLabel* waysValue_ = nullptr;
    QLabel* relationsValue_ = nullptr;
    QLabel* tagsValue_ = nullptr;
    QLabel* featuresValue_ = nullptr;
    QLabel* stageLabel_ = nullptr;
    QPushButton* chooseButton_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QPushButton* openButton_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
    ImportTask* task_ = nullptr;
};

} // namespace p1::app
