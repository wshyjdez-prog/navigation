#include "contracts/ImportedDataset.h"
#include "Sqlite.h"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <utility>

namespace p1::data {
using namespace detail;
namespace {
Tags tags(const QString& text) {
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray())
        throw std::runtime_error("Corrupt imported tags");
    Tags result;
    for (const auto& item : doc.array()) {
        const auto pair = item.toArray();
        if (pair.size() != 2 || !pair[0].isString() || !pair[1].isString())
            throw std::runtime_error("Corrupt imported tag pair");
        result.append(qMakePair(pair[0].toString(), pair[1].toString()));
    }
    return result;
}
void schema(Db& db) {
    Statement s(db, "SELECT value FROM metadata WHERE key='schema'");
    if (!s.row() || s.text(0) != "1") throw std::runtime_error("Unsupported import cache schema");
}
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
    r.ok = true; r.cachePath = path_; r.formatVersion = 1;
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
    r.hasBounds = j["hasBounds"].toBool();
    r.minLat = j["minLat"].toDouble(); r.minLon = j["minLon"].toDouble();
    r.maxLat = j["maxLat"].toDouble(); r.maxLon = j["maxLon"].toDouble();
    for (const auto& v : j["warnings"].toArray()) r.warnings.append(v.toString());
    return r;
}
std::optional<OsmNode> ImportedDataset::node(qint64 id) const {
    Db db(path_, true); schema(db);
    Statement s(db, "SELECT id,lat,lon,tags FROM nodes WHERE id=?"); s.integer(1,id);
    if (!s.row()) return std::nullopt;
    return OsmNode{s.integer(0),s.real(1),s.real(2),tags(s.text(3))};
}
QVector<OsmNode> ImportedDataset::nodes(qint64 offset, int limit) const {
    if (offset < 0 || limit < 1 || limit > 1000) throw std::invalid_argument("Invalid node page");
    Db db(path_, true); schema(db);
    Statement s(db, "SELECT id,lat,lon,tags FROM nodes ORDER BY id LIMIT ? OFFSET ?");
    s.integer(1,limit); s.integer(2,offset);
    QVector<OsmNode> result;
    while(s.row()) result.append(OsmNode{s.integer(0),s.real(1),s.real(2),tags(s.text(3))});
    return result;
}
std::optional<OsmWay> ImportedDataset::way(qint64 id) const {
    Db db(path_, true); schema(db);
    Statement s(db,"SELECT tags FROM ways WHERE id=?"); s.integer(1,id);
    if (!s.row()) return std::nullopt;
    OsmWay w{id,{},tags(s.text(0))};
    Statement refs(db,"SELECT node_id FROM way_nodes WHERE way_id=? ORDER BY seq"); refs.integer(1,id);
    while(refs.row()) w.nodes.append(refs.integer(0));
    return w;
}
std::optional<OsmRelation> ImportedDataset::relation(qint64 id) const {
    Db db(path_, true); schema(db);
    Statement s(db,"SELECT tags FROM relations WHERE id=?"); s.integer(1,id);
    if (!s.row()) return std::nullopt;
    OsmRelation r{id,{},tags(s.text(0))};
    Statement refs(db,"SELECT type,member_id,role FROM relation_members WHERE relation_id=? ORDER BY seq");
    refs.integer(1,id);
    while(refs.row()) r.members.append(OsmMember{int(refs.integer(0)),refs.integer(1),refs.text(2)});
    return r;
}
QString defaultImportCacheDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/imports";
}
QVector<app::ImportResult> listImportedDatasets(const QString& directory) {
    QVector<app::ImportResult> result;
    const auto files = QDir(directory).entryInfoList({"*.sqlite"},QDir::Files,QDir::Time);
    for (const auto& file : files) result.append(ImportedDataset(file.absoluteFilePath()).summary());
    return result;
}
}
