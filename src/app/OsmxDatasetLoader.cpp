#include "app/OsmxDatasetLoader.h"

#include "data/osmx/OsmxCache.h"
#include "data/osmx/OsmxParser.h"

#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <utility>

namespace p1::app {
namespace {

QString shorten(const QString& text) {
    return text.size() > 12 ? text.left(12) + QStringLiteral("…") : text;
}

} // namespace

OsmxDatasetLoader::OsmxDatasetLoader(ImportResult entry, data::OsmxStore* store)
    : entry_(std::move(entry)), store_(store == nullptr ? &data::OsmxStore::shared() : store) {}

ImportResult OsmxDatasetLoader::parse(const QString& path, const ImportProgressCallback& progress,
                                      const ImportLogCallback& log,
                                      const ImportCancelCallback& cancelled) const {
    // The path comes from ImportTask; the cache layout is authoritative so a
    // wrong path can never load a file outside <cachePath>/<datasetId>/.
    Q_UNUSED(path);
    ImportResult result;
    QElapsedTimer timer;
    timer.start();

    auto report = [&progress](int percent, const QString& stage) {
        if (progress) progress(percent, stage);
    };
    auto message = [&log](const QString& text) {
        if (log) log(text);
    };
    auto isCancelled = [&cancelled] { return cancelled && cancelled(); };

    const QString datasetId = entry_.datasetId;
    if (!data::OsmxCache::isSafeDatasetId(datasetId)) {
        result.error = QStringLiteral("数据集 ID 不合法，无法从缓存加载：%1").arg(datasetId);
        message(result.error);
        result.elapsedMs = timer.elapsed();
        return result;
    }
    if (entry_.cachePath.isEmpty()) {
        result.error = QStringLiteral("数据集缺少缓存目录，无法加载。");
        message(result.error);
        result.elapsedMs = timer.elapsed();
        return result;
    }

    // entry_.cachePath is the dataset directory (<root>/<datasetId>), the same
    // value OsmxCache::list() reports, so data.osmx sits directly inside it.
    const QString dataPath = QDir(entry_.cachePath).absoluteFilePath(QStringLiteral("data.osmx"));
    const QFileInfo dataFile(dataPath);
    if (!dataFile.isFile() || !dataFile.isReadable()) {
        result.error = QStringLiteral("缓存数据不存在或不可读：%1").arg(dataPath);
        message(result.error);
        result.elapsedMs = timer.elapsed();
        return result;
    }
    if (entry_.fileSize > 0 && dataFile.size() != entry_.fileSize) {
        result.error = QStringLiteral("缓存数据大小与清单不一致（清单 %1 字节，实际 %2 字节）。")
                           .arg(entry_.fileSize)
                           .arg(dataFile.size());
        message(result.error);
        result.elapsedMs = timer.elapsed();
        return result;
    }

    result.filePath = dataPath;
    result.fileName = entry_.fileName;
    result.fileSize = dataFile.size();
    result.sourceFormat = QStringLiteral("osmx");
    result.datasetId = datasetId;
    result.cachePath = entry_.cachePath;
    result.importedAt = entry_.importedAt;

    report(0, QStringLiteral("读取本地 OSMX 数据集"));
    data::OsmxParseOptions options;
    options.datasetId = datasetId;
    options.importedAt = entry_.importedAt;
    data::OsmxParseCallbacks callbacks;
    callbacks.cancelled = isCancelled;
    callbacks.log = [&message](const QString& text) { message(text); };
    callbacks.progress = [&report](int percent, const QString& stage) {
        report(std::clamp(percent, 0, 99), stage);
    };

    const data::OsmxParseResult parsed = data::OsmxParser().parse(dataPath, options, callbacks);
    if (!parsed.ok()) {
        result.cancelled = parsed.cancelled;
        result.error = parsed.cancelled ? QStringLiteral("加载已取消；未发布数据集。") : parsed.error.toString();
        if (!result.error.isEmpty()) message(result.error);
        result.elapsedMs = timer.elapsed();
        return result;
    }
    if (!entry_.sourceSha256.isEmpty() && parsed.sha256.compare(entry_.sourceSha256, Qt::CaseInsensitive) != 0) {
        result.error = QStringLiteral("缓存数据 SHA-256 与清单不一致（清单 %1，实际 %2），已拒绝发布。")
                           .arg(shorten(entry_.sourceSha256), shorten(parsed.sha256));
        message(result.error);
        result.elapsedMs = timer.elapsed();
        return result;
    }

    const auto& dataset = *parsed.dataset;
    const auto& info = dataset.info();
    store_->publish(parsed.dataset);

    result.ok = true;
    result.formatVersion = info.formatVersion;
    result.sourceName = QStringLiteral("OSMX v1（本地缓存）");
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
    message(QStringLiteral("已从本机缓存加载数据集 %1：可导航道路 %2 条、有向边 %3 条；"
                           "算法与渲染通过 OsmxStore 读取同一实例。")
                .arg(datasetId)
                .arg(info.routableWayCount)
                .arg(info.directedEdgeCount));
    return result;
}

} // namespace p1::app
