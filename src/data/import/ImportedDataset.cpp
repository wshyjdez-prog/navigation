#include "contracts/ImportedDataset.h"
#include "FeatureRules.h"
#include "Sqlite.h"
#include "data/osmx/OsmxTypes.h"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <utility>

namespace p1::data {
using namespace detail;
namespace {
QString schemaVersion(Db& db) {
    Statement s(db, "SELECT value FROM metadata WHERE key='schema'");
    return s.row() ? s.text(0) : QString();
}
void schema(Db& db) {
    if (schemaVersion(db) != kCacheSchema)
        throw std::runtime_error("导入缓存版本不受支持，请重新导入源文件");
}
OsmPoi poi(const Statement& s) {
    return OsmPoi{int(s.integer(0)), s.integer(1), s.real(2), s.real(3), s.text(4), s.text(5), s.text(6),
                  decodeTags(s.text(7))};
}
const char kPoiColumns[] = "SELECT osm_type,osm_id,lat,lon,category,kind,name,tags FROM pois ";
}
ImportedDataset::ImportedDataset(QString path) : path_(std::move(path)) {}
app::ImportResult ImportedDataset::summary() const {
    Db db(path_, true); schema(db);
    Statement query(db, "SELECT value FROM metadata WHERE key='summary'");
    if (!query.row()) throw std::runtime_error("Missing import summary");
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(query.text(0).toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        throw std::runtime_error("Corrupt import summary");
    const auto j = doc.object();
    app::ImportResult r;
    r.ok = true; r.cachePath = path_; r.formatVersion = 2;
    r.fileName = j["name"].toString(); r.filePath = j["sourcePath"].toString();
    r.sourceFormat = j["format"].toString(); r.sourceName = j["generator"].toString();
    r.datasetId = j["datasetId"].toString(); r.sourceSha256 = j["sha256"].toString();
    r.importedAt = j["importedAt"].toString(); r.fileSize = j["bytes"].toString().toLongLong();
    r.elapsedMs = j["elapsedMs"].toString().toLongLong();
    r.stats.nodes = j["nodes"].toString().toULongLong();
    r.stats.ways = j["ways"].toString().toULongLong();
    r.stats.relations = j["relations"].toString().toULongLong();
    r.stats.tags = j["tags"].toString().toULongLong();
    r.stats.nodeReferences = j["nodeReferences"].toString().toULongLong();
    r.stats.relationMembers = j["relationMembers"].toString().toULongLong();
    r.stats.pois = j["pois"].toString().toULongLong();
    r.stats.roads = j["roads"].toString().toULongLong();
    r.stats.areas = j["areas"].toString().toULongLong();
    r.stats.areasSkipped = j["areasSkipped"].toString().toULongLong();
    r.stats.poisMerged = j["poisMerged"].toString().toULongLong();
    r.hasBounds = j["hasBounds"].toBool();
    r.minLat = j["minLat"].toDouble(); r.minLon = j["minLon"].toDouble();
    r.maxLat = j["maxLat"].toDouble(); r.maxLon = j["maxLon"].toDouble();
    for (const auto& v : j["warnings"].toArray()) r.warnings.append(v.toString());
    return r;
}
QVector<OsmPoi> ImportedDataset::pois(const OsmxBounds& b, const QString& category, int limit) const {
    if (!b.valid || !(b.minLat <= b.maxLat) || !(b.minLon <= b.maxLon) || limit < 1 || limit > 10000)
        throw std::invalid_argument("Invalid POI query");
    Db db(path_, true); schema(db);
    const QByteArray sql = QByteArray(kPoiColumns) + "WHERE lat BETWEEN ? AND ? AND lon BETWEEN ? AND ? " +
        (category.isEmpty() ? "" : "AND category=? ") + "ORDER BY id LIMIT ?";
    Statement s(db, sql.constData());
    int n = 0;
    s.real(++n, b.minLat); s.real(++n, b.maxLat); s.real(++n, b.minLon); s.real(++n, b.maxLon);
    if (!category.isEmpty()) s.text(++n, category);
    s.integer(++n, limit);
    QVector<OsmPoi> result;
    while (s.row()) result.append(poi(s));
    return result;
}
QVector<OsmPoi> ImportedDataset::findPois(const QString& nameText, int limit) const {
    if (nameText.trimmed().isEmpty() || limit < 1 || limit > 1000) throw std::invalid_argument("Invalid POI search");
    Db db(path_, true); schema(db);
    // instr() on lower-cased text: no LIKE wildcards to escape. SQLite folds
    // ASCII only, which is enough here because CJK has no letter case.
    const QByteArray sql = QByteArray(kPoiColumns) + "WHERE instr(lower(name),lower(?))>0 ORDER BY id LIMIT ?";
    Statement s(db, sql.constData());
    s.text(1, nameText.trimmed()); s.integer(2, limit);
    QVector<OsmPoi> result;
    while (s.row()) result.append(poi(s));
    return result;
}
QString defaultImportCacheDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/imports";
}
QVector<app::ImportResult> listImportedDatasets(const QString& directory) {
    QVector<app::ImportResult> result;
    const auto files = QDir(directory).entryInfoList({"*.sqlite"},QDir::Files,QDir::Time);
    for (const auto& file : files) {
        { Db db(file.absoluteFilePath(), true); if (schemaVersion(db) == "1") continue; }
        result.append(ImportedDataset(file.absoluteFilePath()).summary());
    }
    return result;
}
}
