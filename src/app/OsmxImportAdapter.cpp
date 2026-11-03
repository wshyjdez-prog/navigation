#include "app/OsmxImportAdapter.h"

#include "data/osmx/OsmxCache.h"
#include "data/osmx/OsmxParser.h"

#include <QElapsedTimer>
#include <QFileInfo>

#include <algorithm>
#include <utility>

namespace p1::app {

OsmxImportAdapter::OsmxImportAdapter(QString cacheDirectory, data::OsmxStore* store)
    : directory_(cacheDirectory.isEmpty() ? data::OsmxCache::defaultDirectory() : std::move(cacheDirectory)),
      store_(store == nullptr ? &data::OsmxStore::shared() : store) {}

QString OsmxImportAdapter::inputFormat(const QString& path) {
    return data::OsmxParser::inputFormat(path);
}

ImportResult OsmxImportAdapter::parse(const QString& path, const ImportProgressCallback& progress,
                                      const ImportLogCallback& log,
                                      const ImportCancelCallback& cancelled) const {
    ImportResult result;
    QElapsedTimer timer;
    timer.start();
    const QFileInfo before(path);
    result.filePath = before.absoluteFilePath();
    result.fileName = before.fileName();
    result.fileSize = before.size();
    result.sourceFormat = QStringLiteral("osmx");

    auto report = [&progress](int percent, const QString& stage) {
        if (progress) progress(percent, stage);
    };
    auto message = [&log](const QString& text) {
        if (log) log(text);
    };
    auto isCancelled = [&cancelled] { return cancelled && cancelled(); };

    report(0, QStringLiteral("读取 OSMX 路网"));
    data::OsmxParseOptions options;
    data::OsmxParseCallbacks callbacks;
    callbacks.cancelled = isCancelled;
    callbacks.log = [&message](const QString& text) { message(text); };
    callbacks.progress = [&report](int percent, const QString& stage) {
        // The parser owns 0..88 %; cache installation owns the rest.
        report(std::clamp(static_cast<int>(percent * 0.88), 0, 88), stage);
    };

    const data::OsmxParseResult parsed = data::OsmxParser().parse(path, options, callbacks);
    if (!parsed.ok()) {
        result.cancelled = parsed.cancelled;
        result.error = parsed.cancelled ? QStringLiteral("导入已取消；未发布数据集。")
                                        : parsed.error.toString();
        if (!result.error.isEmpty()) message(result.error);
        result.elapsedMs = timer.elapsed();
        return result;
    }
    const auto& dataset = *parsed.dataset;
    const auto& info = dataset.info();

    data::OsmxParseCallbacks installCallbacks;
    installCallbacks.cancelled = isCancelled;
    installCallbacks.log = [&message](const QString& text) { message(text); };
    installCallbacks.progress = [&report](int percent, const QString& stage) {
        report(88 + std::clamp(percent * 11 / 100, 0, 11), stage);
    };
    const data::OsmxInstallResult installed =
        data::OsmxCache::install(dataset, parsed.sourcePath, directory_, installCallbacks);
    if (!installed.ok) {
        result.cancelled = installed.cancelled;
        result.error = installed.cancelled ? QStringLiteral("导入已取消；未发布数据集。")
                                           : QStringLiteral("缓存发布失败：%1").arg(installed.error);
        message(result.error);
        result.elapsedMs = timer.elapsed();
        return result;
    }

    store_->publish(parsed.dataset);

    result.ok = true;
    result.formatVersion = info.formatVersion;
    result.sourceName = QStringLiteral("OSMX v1");
    result.datasetId = info.datasetId;
    result.cachePath = installed.datasetDirectory;
    result.sourceSha256 = parsed.sha256;
    result.importedAt = info.importedAt;
    result.hasBounds = info.bounds.valid;
    result.minLat = info.bounds.minLat;
    result.minLon = info.bounds.minLon;
    result.maxLat = info.bounds.maxLat;
    result.maxLon = info.bounds.maxLon;
    result.warnings = parsed.warnings;
    result.stats.nodes = dataset.nodeCount();
    result.stats.ways = dataset.wayCount();
    result.stats.relations = dataset.relationCount();
    result.stats.nodeReferences = info.wayNodeReferenceCount;
    result.stats.relationMembers = info.relationMemberCount;
    result.stats.tags = info.nodeTagCount + info.wayTagCount + info.relationTagCount;
    result.stats.routableWays = info.routableWayCount;
    result.stats.directedEdges = info.directedEdgeCount;
    result.elapsedMs = timer.elapsed();
    report(100, QStringLiteral("加载完成"));
    message(QStringLiteral("统一数据已发布：数据集 %1，可导航道路 %2 条，有向边 %3 条；"
                           "算法与渲染可通过 OsmxStore 读取同一实例。")
                .arg(info.datasetId)
                .arg(info.routableWayCount)
                .arg(info.directedEdgeCount));
    return result;
}

} // namespace p1::app
