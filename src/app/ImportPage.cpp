#include "app/ImportPage.h"

#include "app/ImportFactory.h"
#include "app/ImportParser.h"
#include "app/ImportTask.h"
#include "contracts/ImportedDataset.h"

#include "ui/common/Components.h"
#include "ui/common/Theme.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyle>
#include <QTime>
#include <QListWidget>
#include <QTabWidget>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QVBoxLayout>

#include <memory>
#include <utility>

namespace p1::app {
namespace {

QString formatSize(qint64 bytes) {
    constexpr double kb = 1024.0;
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QStringLiteral("%1 KB").arg(QString::number(bytes / kb, 'f', 1));
    if (bytes < 1024LL * 1024 * 1024) return QStringLiteral("%1 MB").arg(QString::number(bytes / (kb * kb), 'f', 1));
    return QStringLiteral("%1 GB").arg(QString::number(bytes / (kb * kb * kb), 'f', 2));
}

QString formatCount(quint64 value) {
    return QLocale().toString(static_cast<qulonglong>(value));
}

QString formatCoordinate(double value) {
    return QString::number(value, 'f', 6);
}

} // namespace

ImportPage::ImportPage(ui::Theme& theme, QWidget* parent, const QString& cacheDirectory)
    : QWidget(parent), theme_(theme), cacheDirectory_(cacheDirectory.isEmpty()?data::defaultImportCacheDirectory():cacheDirectory) {
    setObjectName("importPage");
    buildUi();

    connect(chooseButton_, &QPushButton::clicked, this, &ImportPage::chooseFile);
    connect(startButton_, &QPushButton::clicked, this, &ImportPage::startImport);
    connect(cancelButton_, &QPushButton::clicked, this, &ImportPage::cancelImport);
    connect(datasetList_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (!importing_ && row >= 0 && row < datasets_.size()) showSummary(datasets_[row]);
    });
    refreshCatalog();
}

ImportPage::~ImportPage() {
    if (task_ != nullptr) {
        task_->disconnect(this);
        task_->cancel();
        task_->wait();
        delete task_;
        task_ = nullptr;
    }
}

void ImportPage::buildUi() {
    auto* columns = new QHBoxLayout(this);
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(24);

    auto* leftContent = new QWidget;
    auto* left = new QVBoxLayout(leftContent);
    left->setContentsMargins(0, 0, 6, 12);
    left->setSpacing(24);

    auto* picker = new ui::Card(theme_);
    picker->setObjectName("importPickerCard");
    auto* pickerBody = picker->body();
    pickerBody->addWidget(ui::label(QStringLiteral("01  /  添加数据"), "eyebrow"));
    pickerBody->addWidget(ui::label(QStringLiteral("导入本地地图数据"), "sectionTitle"));
    pickerBody->addWidget(ui::label(QStringLiteral("支持 PBF、OSM XML，以及 gzip/bzip2 压缩的 OSM 文件。"),
                                    "secondary"));

    fileName_ = ui::label(QStringLiteral("尚未选择文件"), "cardTitle");
    fileName_->setObjectName("selectedFileName");
    fileInfo_ = ui::label(QStringLiteral("原文件保持不变；解析结果保存到本机，可在下次启动时查看。"), "secondary");
    fileName_->setTextFormat(Qt::PlainText); fileInfo_->setTextFormat(Qt::PlainText);
    pickerBody->addWidget(fileName_);
    pickerBody->addWidget(fileInfo_);

    chooseButton_ = ui::button(QStringLiteral("选择文件"), true);
    chooseButton_->setObjectName("chooseFileButton");
    chooseButton_->setAccessibleName(QStringLiteral("选择要导入的 OpenStreetMap 文件"));
    pickerBody->addWidget(chooseButton_);

    startButton_ = ui::button(QStringLiteral("开始导入"));
    startButton_->setObjectName("startImportButton");
    startButton_->setAccessibleName(QStringLiteral("开始导入所选文件"));
    startButton_->setEnabled(false);
    startButton_->setToolTip(QStringLiteral("请先选择文件"));
    pickerBody->addWidget(startButton_);

    cancelButton_ = ui::button(QStringLiteral("取消导入"));
    cancelButton_->setObjectName("cancelImportButton");
    cancelButton_->setEnabled(false);
    pickerBody->addWidget(cancelButton_);

    statusBadge_ = ui::badge(QStringLiteral("!  尚未选择文件"), "warning");
    pickerBody->addWidget(statusBadge_);
    left->addWidget(picker);

    auto* details = new ui::Card(theme_);
    details->body()->addWidget(ui::label(QStringLiteral("文件信息"), "cardTitle"));
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(1, 1);
    auto addRow = [grid](int row, const QString& name, QLabel*& value) {
        grid->addWidget(ui::label(name, "caption"), row, 0, Qt::AlignTop);
        value = ui::label(QStringLiteral("—"), "secondary");
        grid->addWidget(value, row, 1);
    };
    addRow(0, QStringLiteral("格式版本"), versionValue_);
    addRow(1, QStringLiteral("数据来源"), sourceValue_);
    addRow(2, QStringLiteral("覆盖范围"), boundsValue_);
    addRow(3, QStringLiteral("标签数量"), tagsValue_);
    addRow(4, QStringLiteral("解析用时"), durationValue_);
    sourceValue_->setTextFormat(Qt::PlainText);
    details->body()->addLayout(grid);
    left->addWidget(details);
    left->addStretch();

    auto* leftScroll = ui::scrollable(leftContent);
    leftScroll->setObjectName("importTaskPanel");
    columns->addWidget(leftScroll);

    auto* rightContent = new QWidget;
    auto* right = new QVBoxLayout(rightContent);
    right->setContentsMargins(6, 0, 6, 12);
    right->setSpacing(24);

    auto* metrics = new QHBoxLayout;
    metrics->setSpacing(16);
    auto makeMetric = [this](const QString& title, QLabel*& value) {
        auto* card = new ui::Card(theme_);
        card->body()->setContentsMargins(16, 16, 16, 16);
        card->body()->setSpacing(8);
        card->body()->addWidget(ui::label(title, "secondary"));
        value = ui::label(QStringLiteral("—"), "metric");
        card->body()->addWidget(value);
        card->body()->addWidget(ui::label(QStringLiteral("本次导入"), "caption"));
        return card;
    };
    metrics->addWidget(makeMetric(QStringLiteral("节点"), nodesValue_));
    metrics->addWidget(makeMetric(QStringLiteral("道路"), waysValue_));
    metrics->addWidget(makeMetric(QStringLiteral("关系"), relationsValue_));
    right->addLayout(metrics);

    auto* progressCard = new ui::Card(theme_);
    progressCard->body()->addWidget(ui::label(QStringLiteral("导入进度"), "sectionTitle"));
    progressBar_ = new QProgressBar;
    progressBar_->setObjectName("importProgress");
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setTextVisible(false);
    progressBar_->setAccessibleName(QStringLiteral("导入进度"));
    stageLabel_ = ui::label(QStringLiteral("等待开始"), "secondary");
    stageLabel_->setTextFormat(Qt::PlainText);
    progressCard->body()->addWidget(progressBar_);
    progressCard->body()->addWidget(stageLabel_);
    right->addWidget(progressCard);

    auto* logCard = new ui::Card(theme_);
    logCard->body()->addWidget(ui::label(QStringLiteral("本地数据与导入详情"), "sectionTitle"));
    detailsTabs_ = new QTabWidget;
    datasetList_ = new QListWidget;
    datasetList_->setObjectName("importedDatasets");
    datasetList_->setAccessibleName("本机已导入的数据集");
    detailsTabs_->addTab(datasetList_, "本地数据集");
    logView_ = new QPlainTextEdit;
    logView_->setObjectName("importLog");
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(2000);
    logView_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    logView_->setAccessibleName(QStringLiteral("导入日志"));
    logView_->setPlaceholderText(QStringLiteral("导入开始后在此显示阶段、统计与错误信息。"));
    logView_->setMinimumHeight(180);
    logView_->setFrameShape(QFrame::NoFrame);
    detailsTabs_->addTab(logView_, "当前导入信息");
    logCard->body()->addWidget(detailsTabs_, 1);
    right->addWidget(logCard, 1);

    columns->addWidget(rightContent, 1);

    auto applyLogStyle = [this] {
        const auto& colors = theme_.colors();
        logView_->setStyleSheet(QStringLiteral(
                                    "QPlainTextEdit { background: %1; color: %2; border: 1px solid %3;"
                                    " border-radius: 8px; padding: 8px; }")
                                    .arg(colors.raised)
                                    .arg(colors.text)
                                    .arg(colors.border));
    };
    applyLogStyle();
    connect(&theme_, &ui::Theme::changed, this, applyLogStyle);
}

void ImportPage::selectFile(const QString& path) {
    if (importing_) return;
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || !info.isReadable()) {
        fileInfo_->setText(QStringLiteral("无法读取所选文件，请检查路径与文件权限后重新选择。"));
        setStatusBadge(QStringLiteral("!  文件不可读"), "warning");
        startButton_->setEnabled(false);
        startButton_->setToolTip(QStringLiteral("请先选择可读取的文件"));
        appendLog(QStringLiteral("选择文件失败：%1").arg(path));
        return;
    }

    selectedPath_ = info.absoluteFilePath();
    fileName_->setText(info.fileName());
    fileName_->setToolTip(selectedPath_);
    if (ImportFactory::supports(selectedPath_)) {
        fileInfo_->setText(QStringLiteral("%1 · OpenStreetMap 数据文件").arg(formatSize(info.size())));
        setStatusBadge(QStringLiteral("!  待导入"), "warning");
    } else {
        const QString suffix = info.suffix();
        fileInfo_->setText(QStringLiteral("%1 · 不支持扩展名“%2”。请选择 .osm、.pbf、.osm.gz 或 .osm.bz2 文件。")
                               .arg(formatSize(info.size()))
                               .arg(suffix.isEmpty() ? QStringLiteral("无") : suffix));
        setStatusBadge(QStringLiteral("!  格式可能不受支持"), "warning");
    }
    resetResultView();
    startButton_->setEnabled(ImportFactory::supports(selectedPath_));
    startButton_->setToolTip(ImportFactory::supports(selectedPath_) ? "开始导入所选文件" : "所选格式不受支持");
    appendLog(QStringLiteral("已选择文件：%1（%2）").arg(selectedPath_).arg(formatSize(info.size())));
}

void ImportPage::startImport() {
    if (importing_) return;
    if (selectedPath_.isEmpty()) {
        appendLog(QStringLiteral("尚未选择文件，无法开始导入。"));
        QMessageBox::information(this, QStringLiteral("尚未选择文件"),
                                 QStringLiteral("请先选择一个 OpenStreetMap 数据文件。"));
        return;
    }

    std::unique_ptr<ImportParser> parser = ImportFactory::createParser(selectedPath_,cacheDirectory_);
    if (!parser) {
        const QFileInfo info(selectedPath_);
        const QString suffix = info.suffix();
        const QString message = QStringLiteral("暂不支持该文件格式（%1）。当前支持：%2。")
                                    .arg(suffix.isEmpty() ? QStringLiteral("无扩展名") : suffix)
                                    .arg(ImportFactory::supportedExtensions().join(QStringLiteral(", ")));
        appendLog(message);
        setStatusBadge(QStringLiteral("!  格式不受支持"), "error");
        QMessageBox::warning(this, QStringLiteral("无法导入"), message);
        return;
    }

    resetResultView();
    detailsTabs_->setCurrentWidget(logView_);
    importing_ = true;
    setBusy(true);
    setStatusBadge(QStringLiteral("…  正在解析"), "info");
    stageLabel_->setText(QStringLiteral("准备读取文件…"));

    const QFileInfo info(selectedPath_);
    appendLog(QStringLiteral("开始导入 %1（%2）").arg(info.fileName()).arg(formatSize(info.size())));

    task_ = new ImportTask(std::move(parser), selectedPath_, this);
    connect(task_, &ImportTask::progressChanged, this, &ImportPage::onProgress);
    connect(task_, &ImportTask::logMessage, this, &ImportPage::onLog);
    connect(task_, &ImportTask::completed, this, &ImportPage::onCompleted);
    task_->start();
}

void ImportPage::cancelImport() {
    if (task_ == nullptr) return;
    cancelButton_->setEnabled(false);
    setStatusBadge(QStringLiteral("…  正在取消"), "warning");
    stageLabel_->setText(QStringLiteral("正在取消…"));
    appendLog(QStringLiteral("已请求取消，等待解析线程停止…"));
    task_->cancel();
}

void ImportPage::onProgress(int percent, const QString& stage) {
    if (progressBar_ == nullptr) return;
    progressBar_->setValue(qBound(0, percent, 100));
    stageLabel_->setText(QStringLiteral("%1 · %2%").arg(stage).arg(qBound(0, percent, 100)));
}

void ImportPage::onLog(const QString& message) {
    appendLog(message);
}

void ImportPage::onCompleted(const ImportResult& result) {
    if (task_ != nullptr) {
        // The worker thread has emitted its last signal; join it before the
        // thread object is destroyed so the page can be closed safely.
        task_->wait();
        task_->deleteLater();
        task_ = nullptr;
    }
    importing_ = false;
    setBusy(false);

    if (result.cancelled) {
        setStatusBadge(QStringLiteral("!  已取消"), "warning");
        stageLabel_->setText(QStringLiteral("导入已取消"));
        appendLog(QStringLiteral("导入已取消，可以重新选择文件或再次开始。"));
        emit importFinished(result);
        return;
    }

    if (!result.ok) {
        progressBar_->setValue(0);
        setStatusBadge(QStringLiteral("!  导入失败"), "error");
        stageLabel_->setText(QStringLiteral("导入失败：%1").arg(result.error));
        appendLog(QStringLiteral("导入失败：%1").arg(result.error));
        emit importFinished(result);
        return;
    }

    progressBar_->setValue(100);
    setStatusBadge(result.warnings.isEmpty()?QStringLiteral("✓  已保存到本机"):QStringLiteral("!  已保存，含数据警告"), result.warnings.isEmpty()?"success":"warning");
    stageLabel_->setText(result.warnings.isEmpty()?QStringLiteral("导入完成 · 用时 %1 ms").arg(result.elapsedMs):result.warnings.join('\n'));
    showSummary(result);
    refreshCatalog();
    detailsTabs_->setCurrentWidget(datasetList_);
    emit importFinished(result);
}

void ImportPage::showSummary(const ImportResult& result) {

    nodesValue_->setText(formatCount(result.stats.nodes));
    waysValue_->setText(formatCount(result.stats.ways));
    relationsValue_->setText(formatCount(result.stats.relations));
    tagsValue_->setText(formatCount(result.stats.tags));

    versionValue_->setText(result.sourceFormat);
    if (result.sourceName.isEmpty()) {
        sourceValue_->setText(QStringLiteral("未提供"));
        sourceValue_->setToolTip(QString());
    } else {
        sourceValue_->setText(result.sourceName);
        sourceValue_->setToolTip(result.sourceName);
    }
    if (result.hasBounds) {
        boundsValue_->setText(QStringLiteral("%1, %2 — %3, %4")
                                  .arg(formatCoordinate(result.minLat))
                                  .arg(formatCoordinate(result.minLon))
                                  .arg(formatCoordinate(result.maxLat))
                                  .arg(formatCoordinate(result.maxLon)));
    } else {
        boundsValue_->setText(QStringLiteral("未提供"));
    }
    durationValue_->setText(QStringLiteral("%1 ms").arg(result.elapsedMs));

}

void ImportPage::chooseFile() {
    if (importing_) return;
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择 OpenStreetMap 数据文件"),
                                                     QDir::homePath(), ImportFactory::fileDialogFilter());
    if (path.isEmpty()) return;
    selectFile(path);
}

void ImportPage::resetResultView() {
    progressBar_->setValue(0);
    stageLabel_->setText(QStringLiteral("等待开始"));
    if (nodesValue_ != nullptr) {
        nodesValue_->setText(QStringLiteral("—"));
        waysValue_->setText(QStringLiteral("—"));
        relationsValue_->setText(QStringLiteral("—"));
        tagsValue_->setText(QStringLiteral("—"));
        versionValue_->setText(QStringLiteral("—"));
        sourceValue_->setText(QStringLiteral("—"));
        boundsValue_->setText(QStringLiteral("—"));
        durationValue_->setText(QStringLiteral("—"));
    }
}

void ImportPage::setStatusBadge(const QString& text, const char* tone) {
    if (statusBadge_ == nullptr) return;
    statusBadge_->setText(text);
    statusBadge_->setProperty("badge", QString::fromLatin1(tone));
    statusBadge_->style()->unpolish(statusBadge_);
    statusBadge_->style()->polish(statusBadge_);
}

void ImportPage::appendLog(const QString& message) {
    if (logView_ == nullptr) return;
    logView_->appendPlainText(QStringLiteral("[%1] %2")
                                  .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")))
                                  .arg(message));
}

void ImportPage::setBusy(bool busy) {
    chooseButton_->setEnabled(!busy);
    cancelButton_->setEnabled(busy);
    startButton_->setEnabled(!busy && ImportFactory::supports(selectedPath_));
    datasetList_->setEnabled(!busy);
    if (busy) {
        startButton_->setToolTip(QStringLiteral("导入进行中"));
    } else if (selectedPath_.isEmpty()) {
        startButton_->setToolTip(QStringLiteral("请先选择文件"));
    } else {
        startButton_->setToolTip(QStringLiteral("开始导入所选文件"));
    }
}

void ImportPage::refreshCatalog() {
    const int generation=++catalogGeneration_;
    auto* watcher=new QFutureWatcher<QVector<ImportResult>>(this);
    connect(watcher,&QFutureWatcher<QVector<ImportResult>>::finished,this,[this,watcher,generation] {
        watcher->deleteLater();
        if(generation!=catalogGeneration_) return;
        try {
            datasets_=watcher->result(); datasetList_->clear();
            for(const auto& r:datasets_) {
                auto* item=new QListWidgetItem(QString("%1 · %2\n节点 %3 / 道路 %4 / 关系 %5 · %6")
                    .arg(r.fileName,r.sourceFormat,formatCount(r.stats.nodes),formatCount(r.stats.ways),formatCount(r.stats.relations),r.warnings.isEmpty()?"已保存":"已保存，含数据警告"),datasetList_);
                item->setToolTip(r.cachePath+"\n"+r.importedAt+"\n"+r.warnings.join('\n'));
            }
            if(datasets_.isEmpty()) datasetList_->addItem("尚无导入数据。选择文件并开始导入。");
            else if (!importing_) {
                datasetList_->setCurrentRow(0);
                detailsTabs_->setCurrentWidget(datasetList_);
            }
        } catch(const std::exception& e) {
            appendLog(QString("读取本地数据集失败：%1").arg(QString::fromUtf8(e.what())));
        }
    });
    const auto directory=cacheDirectory_;
    watcher->setFuture(QtConcurrent::run([directory] { return data::listImportedDatasets(directory); }));
}

void ImportPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (auto* panel = findChild<QWidget*>(QStringLiteral("importTaskPanel"))) {
        panel->setFixedWidth(window()->width() < 1280 ? 304 : 360);
    }
}

} // namespace p1::app
