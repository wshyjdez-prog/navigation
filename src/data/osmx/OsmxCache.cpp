#include "OsmxCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace p1::data {
namespace {

constexpr char kCacheSchema[] = "p1-osmx-cache/1";
constexpr qint64 kCopyChunkBytes = 1024 * 1024;

QString numberText(quint64 value) {
    return QString::number(value);
}

qint64 jsonInt64(const QJsonValue& value) {
    if (value.isString()) return value.toString().toLongLong();
    if (value.isDouble()) return static_cast<qint64>(value.toDouble());
    return 0;
}

quint64 jsonUInt64(const QJsonValue& value) {
    if (value.isString()) return value.toString().toULongLong();
    if (value.isDouble()) return static_cast<quint64>(value.toDouble());
    return 0;
}

void report(const OsmxParseCallbacks& callbacks, int percent, const QString& stage) {
    if (callbacks.progress) callbacks.progress(std::clamp(percent, 0, 100), stage);
}

void logMessage(const OsmxParseCallbacks& callbacks, const QString& message) {
    if (callbacks.log) callbacks.log(message);
}

QJsonObject manifestFor(const OsmxDataset& dataset, qint64 elapsedMs) {
    const auto& info = dataset.info();
    QJsonArray warnings;
    for (const auto& warning : info.warnings) warnings.append(warning);
    QJsonObject object{
        {QStringLiteral("schema"), QString::fromLatin1(kCacheSchema)},
        {QStringLiteral("format"), QStringLiteral("osmx")},
        {QStringLiteral("formatVersion"), static_cast<int>(info.formatVersion)},
        {QStringLiteral("reader"), QStringLiteral("p1 OsmxParser v1")},
        {QStringLiteral("name"), info.sourceFileName},
        {QStringLiteral("sourceFile"), info.sourceFileName},
        {QStringLiteral("sourcePath"), info.sourcePath},
        {QStringLiteral("headerSourcePath"), info.headerSourcePath},
        {QStringLiteral("datasetId"), info.datasetId},
        {QStringLiteral("sha256"), info.sourceSha256},
        {QStringLiteral("importedAt"), info.importedAt},
        {QStringLiteral("elapsedMs"), QString::number(elapsedMs)},
        {QStringLiteral("bytes"), QString::number(info.fileBytes)},
        {QStringLiteral("nodes"), numberText(dataset.nodeCount())},
        {QStringLiteral("ways"), numberText(dataset.wayCount())},
        {QStringLiteral("relations"), numberText(dataset.relationCount())},
        {QStringLiteral("tags"), numberText(info.nodeTagCount + info.wayTagCount + info.relationTagCount)},
        {QStringLiteral("nodeTags"), numberText(info.nodeTagCount)},
        {QStringLiteral("wayTags"), numberText(info.wayTagCount)},
        {QStringLiteral("relationTags"), numberText(info.relationTagCount)},
        {QStringLiteral("nodeReferences"), numberText(info.wayNodeReferenceCount)},
        {QStringLiteral("relationMembers"), numberText(info.relationMemberCount)},
        {QStringLiteral("routableWays"), numberText(info.routableWayCount)},
        {QStringLiteral("undirectedSegments"), numberText(info.undirectedSegmentCount)},
        {QStringLiteral("directedEdges"), numberText(info.directedEdgeCount)},
        {QStringLiteral("roadLengthMeters"), QString::number(info.roadLengthMeters, 'f', 3)},
        {QStringLiteral("hasBounds"), info.bounds.valid},
        {QStringLiteral("minLat"), info.bounds.minLat},
        {QStringLiteral("minLon"), info.bounds.minLon},
        {QStringLiteral("maxLat"), info.bounds.maxLat},
        {QStringLiteral("maxLon"), info.bounds.maxLon},
        {QStringLiteral("warnings"), warnings},
    };
    return object;
}

app::ImportResult toImportResult(const QJsonObject& manifest, const QString& datasetDir) {
    app::ImportResult result;
    result.ok = true;
    result.cachePath = datasetDir;
    result.fileName = manifest.value(QStringLiteral("name")).toString();
    if (result.fileName.isEmpty()) result.fileName = manifest.value(QStringLiteral("sourceFile")).toString();
    result.filePath = manifest.value(QStringLiteral("sourcePath")).toString();
    result.sourceFormat = QStringLiteral("osmx");
    result.sourceName = manifest.value(QStringLiteral("reader")).toString();
    result.formatVersion = static_cast<quint32>(jsonUInt64(manifest.value(QStringLiteral("formatVersion"))));
    result.datasetId = manifest.value(QStringLiteral("datasetId")).toString();
    result.sourceSha256 = manifest.value(QStringLiteral("sha256")).toString();
    result.importedAt = manifest.value(QStringLiteral("importedAt")).toString();
    result.fileSize = jsonInt64(manifest.value(QStringLiteral("bytes")));
    result.elapsedMs = jsonInt64(manifest.value(QStringLiteral("elapsedMs")));
    result.stats.nodes = jsonUInt64(manifest.value(QStringLiteral("nodes")));
    result.stats.ways = jsonUInt64(manifest.value(QStringLiteral("ways")));
    result.stats.relations = jsonUInt64(manifest.value(QStringLiteral("relations")));
    result.stats.tags = jsonUInt64(manifest.value(QStringLiteral("tags")));
    result.stats.nodeReferences = jsonUInt64(manifest.value(QStringLiteral("nodeReferences")));
    result.stats.relationMembers = jsonUInt64(manifest.value(QStringLiteral("relationMembers")));
    result.stats.routableWays = jsonUInt64(manifest.value(QStringLiteral("routableWays")));
    result.stats.directedEdges = jsonUInt64(manifest.value(QStringLiteral("directedEdges")));
    result.hasBounds = manifest.value(QStringLiteral("hasBounds")).toBool();
    result.minLat = manifest.value(QStringLiteral("minLat")).toDouble();
    result.minLon = manifest.value(QStringLiteral("minLon")).toDouble();
    result.maxLat = manifest.value(QStringLiteral("maxLat")).toDouble();
    result.maxLon = manifest.value(QStringLiteral("maxLon")).toDouble();
    for (const auto& warning : manifest.value(QStringLiteral("warnings")).toArray()) {
        result.warnings.append(warning.toString());
    }
    return result;
}

} // namespace

QString OsmxCache::defaultDirectory() {
    // Same root as the standard OSM cache: the catalog scans this directory for
    // both *.sqlite files and OSMX dataset directories.
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/imports");
}

bool OsmxCache::isSafeDatasetId(const QString& id) {
    if (id.isEmpty() || id.size() > 64) return false;
    for (const QChar c : id) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('-') && c != QLatin1Char('_')) return false;
    }
    return id != QLatin1String(".") && id != QLatin1String("..");
}

QString OsmxCache::datasetDirectory(const QString& directory, const QString& datasetId) {
    return QDir(directory).absoluteFilePath(datasetId);
}

QString OsmxCache::dataPath(const QString& directory, const QString& datasetId) {
    return QDir(datasetDirectory(directory, datasetId)).absoluteFilePath(QStringLiteral("data.osmx"));
}

QString OsmxCache::manifestPath(const QString& directory, const QString& datasetId) {
    return QDir(datasetDirectory(directory, datasetId)).absoluteFilePath(QStringLiteral("manifest.json"));
}

OsmxInstallResult OsmxCache::install(const OsmxDataset& dataset, const QString& sourceFile,
                                     const QString& directory, const OsmxParseCallbacks& callbacks) {
    OsmxInstallResult result;
    QElapsedTimer timer;
    timer.start();
    const auto& info = dataset.info();
    const QString datasetId = info.datasetId;
    auto fail = [&](const QString& message) {
        result.ok = false;
        result.error = message;
        result.elapsedMs = timer.elapsed();
        return result;
    };
    auto cancelled = [&]() {
        return callbacks.cancelled && callbacks.cancelled();
    };

    if (!isSafeDatasetId(datasetId)) return fail(QStringLiteral("数据集 ID 不合法，无法作为缓存目录名：%1").arg(datasetId));
    if (directory.isEmpty()) return fail(QStringLiteral("缓存目录为空"));
    if (!QDir().mkpath(directory)) return fail(QStringLiteral("无法创建缓存目录：%1").arg(directory));

    const QFileInfo source(sourceFile);
    if (!source.isFile() || !source.isReadable()) return fail(QStringLiteral("源文件不可读：%1").arg(sourceFile));
    if (info.sourceSha256.isEmpty()) return fail(QStringLiteral("数据集缺少 SHA-256，拒绝发布缓存"));

    const QString finalDirectory = datasetDirectory(directory, datasetId);
    if (QDir(finalDirectory).exists()) return fail(QStringLiteral("数据集 %1 已存在").arg(datasetId));

    const QString staging = QDir(directory).absoluteFilePath(
        QStringLiteral(".staging-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!QDir().mkpath(staging)) return fail(QStringLiteral("无法创建导入暂存目录"));
    auto cleanup = [&staging] { QDir(staging).removeRecursively(); };

    report(callbacks, 0, QStringLiteral("写入本地缓存"));
    QFile input(sourceFile);
    if (!input.open(QIODevice::ReadOnly)) {
        cleanup();
        return fail(input.errorString());
    }
    QFile output(QDir(staging).absoluteFilePath(QStringLiteral("data.osmx")));
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        cleanup();
        return fail(output.errorString());
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const qint64 total = source.size();
    qint64 copied = 0;
    while (copied < total) {
        if (cancelled()) {
            input.close();
            output.close();
            cleanup();
            result.cancelled = true;
            result.error = QStringLiteral("写入缓存已取消");
            result.elapsedMs = timer.elapsed();
            return result;
        }
        const QByteArray chunk = input.read(std::min(kCopyChunkBytes, total - copied));
        if (chunk.isEmpty() && input.error() != QFileDevice::NoError) {
            input.close();
            output.close();
            cleanup();
            return fail(input.errorString());
        }
        if (output.write(chunk) != chunk.size()) {
            input.close();
            output.close();
            cleanup();
            return fail(QStringLiteral("写入缓存文件失败：%1").arg(output.errorString()));
        }
        hash.addData(chunk);
        copied += chunk.size();
        report(callbacks, static_cast<int>(copied * 95 / std::max<qint64>(1, total)), QStringLiteral("写入本地缓存"));
    }
    output.flush();
    if (output.error() != QFileDevice::NoError) {
        input.close();
        output.close();
        cleanup();
        return fail(QStringLiteral("缓存文件写入不完整：%1").arg(output.errorString()));
    }
    output.close();
    input.close();

    const QString actualSha = QString::fromLatin1(hash.result().toHex());
    if (actualSha.compare(info.sourceSha256, Qt::CaseInsensitive) != 0) {
        cleanup();
        return fail(QStringLiteral("复制后 SHA-256 与加载结果不一致，源文件可能在导入期间被修改"));
    }
    const QFileInfo after(sourceFile);
    if (after.size() != source.size() || after.lastModified() != source.lastModified()) {
        cleanup();
        return fail(QStringLiteral("写入缓存期间源文件被修改"));
    }

    report(callbacks, 97, QStringLiteral("写入缓存清单"));
    const QJsonObject manifest = manifestFor(dataset, timer.elapsed());
    QSaveFile manifestFile(QDir(staging).absoluteFilePath(QStringLiteral("manifest.json")));
    if (!manifestFile.open(QIODevice::WriteOnly)) {
        cleanup();
        return fail(manifestFile.errorString());
    }
    manifestFile.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented));
    if (!manifestFile.commit()) {
        cleanup();
        return fail(QStringLiteral("提交缓存清单失败：%1").arg(manifestFile.errorString()));
    }

    if (!QDir().rename(staging, finalDirectory)) {
        cleanup();
        return fail(QStringLiteral("无法将缓存目录发布为 %1").arg(finalDirectory));
    }
    result.ok = true;
    result.datasetDirectory = finalDirectory;
    result.elapsedMs = timer.elapsed();
    report(callbacks, 100, QStringLiteral("缓存已写入"));
    logMessage(callbacks, QStringLiteral("已写入本地数据集缓存：%1").arg(finalDirectory));
    return result;
}

OsmxCacheListing OsmxCache::list(const QString& directory) {
    OsmxCacheListing listing;
    const QDir root(directory);
    if (!root.exists()) return listing;
    const auto entries = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (const auto& entry : entries) {
        const QString name = entry.fileName();
        if (name.startsWith(QLatin1Char('.'))) continue;
        if (!isSafeDatasetId(name)) {
            listing.problems.append(QStringLiteral("跳过非法数据集目录：%1").arg(name));
            continue;
        }
        const QString manifestPath = QDir(entry.absoluteFilePath()).absoluteFilePath(QStringLiteral("manifest.json"));
        QFile manifestFile(manifestPath);
        if (!manifestFile.open(QIODevice::ReadOnly)) {
            listing.problems.append(QStringLiteral("%1：缺少可读的 manifest.json").arg(name));
            continue;
        }
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(manifestFile.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            listing.problems.append(QStringLiteral("%1：manifest.json 不是有效 JSON").arg(name));
            continue;
        }
        const QJsonObject manifest = document.object();
        if (manifest.value(QStringLiteral("schema")).toString() != QLatin1String(kCacheSchema)) {
            listing.problems.append(QStringLiteral("%1：缓存 schema 不受支持").arg(name));
            continue;
        }
        if (manifest.value(QStringLiteral("datasetId")).toString() != name) {
            listing.problems.append(QStringLiteral("%1：manifest 的数据集 ID 与目录名不一致").arg(name));
            continue;
        }
        const QString dataPath = QDir(entry.absoluteFilePath()).absoluteFilePath(QStringLiteral("data.osmx"));
        const QFileInfo dataFile(dataPath);
        if (!dataFile.isFile() || !dataFile.isReadable()) {
            listing.problems.append(QStringLiteral("%1：data.osmx 缺失或不可读").arg(name));
            continue;
        }
        const qint64 expectedBytes = jsonInt64(manifest.value(QStringLiteral("bytes")));
        if (expectedBytes > 0 && dataFile.size() != expectedBytes) {
            listing.problems.append(QStringLiteral("%1：data.osmx 大小与清单不一致").arg(name));
            continue;
        }
        listing.entries.append(toImportResult(manifest, entry.absoluteFilePath()));
    }
    std::sort(listing.entries.begin(), listing.entries.end(),
              [](const app::ImportResult& left, const app::ImportResult& right) {
                  return left.importedAt > right.importedAt;
              });
    return listing;
}

bool OsmxCache::remove(const QString& directory, const QString& datasetId, QString* error) {
    if (!isSafeDatasetId(datasetId)) {
        if (error != nullptr) *error = QStringLiteral("数据集 ID 不合法");
        return false;
    }
    const QString target = datasetDirectory(directory, datasetId);
    if (!QFileInfo(target).isDir()) {
        if (error != nullptr) *error = QStringLiteral("数据集不存在");
        return false;
    }
    if (!QDir(target).removeRecursively()) {
        if (error != nullptr) *error = QStringLiteral("删除数据集目录失败");
        return false;
    }
    return true;
}

} // namespace p1::data
