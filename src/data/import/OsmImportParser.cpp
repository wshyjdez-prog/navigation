#include "OsmImportParser.h"
#include "contracts/ImportedDataset.h"
#include "Sqlite.h"
#include <osmium/io/any_input.hpp>
#include <osmium/handler.hpp>
#include <osmium/visitor.hpp>
#include <osmium/version.hpp>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringDecoder>
#include <QTemporaryDir>
#include <QUuid>
#include <cmath>
#include <functional>
#include <utility>

namespace p1::data {
using namespace detail;
namespace {
struct Cancelled {};
QString utf8(const char* text) {
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString value = decoder(QByteArrayView(text));
    if (decoder.hasError()) throw std::runtime_error("OSM contains invalid UTF-8");
    return value;
}
QString encodeTags(const osmium::TagList& tags) {
    QJsonArray values;
    for (const auto& tag : tags) values.append(QJsonArray{utf8(tag.key()),utf8(tag.value())});
    return QString::fromUtf8(QJsonDocument(values).toJson(QJsonDocument::Compact));
}
struct Handler : osmium::handler::Handler {
    Statement nodeInsert, wayInsert, refInsert, relationInsert, memberInsert;
    app::ImportResult& result;
    std::function<void()> check;
    Handler(Db& db, app::ImportResult& r, std::function<void()> callback)
        : nodeInsert(db,"INSERT INTO nodes VALUES(?,?,?,?)"),
          wayInsert(db,"INSERT INTO ways VALUES(?,?)"),
          refInsert(db,"INSERT INTO way_nodes VALUES(?,?,?)"),
          relationInsert(db,"INSERT INTO relations VALUES(?,?)"),
          memberInsert(db,"INSERT INTO relation_members VALUES(?,?,?,?,?)"),
          result(r),check(std::move(callback)) {}
    void object(const osmium::OSMObject& obj) {
        check();
        if (!obj.visible()) throw std::runtime_error("Deleted/history objects are not supported; supply an OSM snapshot");
        if (obj.id() == 0) throw std::runtime_error("OSM object ID must not be zero");
        result.stats.tags += obj.tags().size();
    }
    void node(const osmium::Node& n) {
        object(n);
        if (!n.location().valid()) throw std::runtime_error("OSM node has no valid WGS84 location");
        const double lat=n.location().lat(), lon=n.location().lon();
        if (!std::isfinite(lat) || !std::isfinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180)
            throw std::runtime_error("OSM node coordinates are outside WGS84 bounds");
        nodeInsert.integer(1,n.id()); nodeInsert.real(2,lat); nodeInsert.real(3,lon);
        nodeInsert.text(4,encodeTags(n.tags())); nodeInsert.insert();
        if (!result.hasBounds) {
            result.minLat=result.maxLat=lat; result.minLon=result.maxLon=lon; result.hasBounds=true;
        } else {
            result.minLat=qMin(result.minLat,lat); result.maxLat=qMax(result.maxLat,lat);
            result.minLon=qMin(result.minLon,lon); result.maxLon=qMax(result.maxLon,lon);
        }
        ++result.stats.nodes;
    }
    void way(const osmium::Way& w) {
        object(w);
        wayInsert.integer(1,w.id()); wayInsert.text(2,encodeTags(w.tags())); wayInsert.insert();
        int index=0;
        for (const auto& node : w.nodes()) {
            check(); refInsert.integer(1,w.id()); refInsert.integer(2,index++);
            refInsert.integer(3,node.ref()); refInsert.insert(); ++result.stats.nodeReferences;
        }
        ++result.stats.ways;
    }
    void relation(const osmium::Relation& r) {
        object(r);
        relationInsert.integer(1,r.id()); relationInsert.text(2,encodeTags(r.tags())); relationInsert.insert();
        int index=0;
        for (const auto& member : r.members()) {
            check();
            int type=-1;
            if (member.type()==osmium::item_type::node) type=0;
            if (member.type()==osmium::item_type::way) type=1;
            if (member.type()==osmium::item_type::relation) type=2;
            if (type<0) throw std::runtime_error("Unknown OSM relation member type");
            memberInsert.integer(1,r.id()); memberInsert.integer(2,index++); memberInsert.integer(3,type);
            memberInsert.integer(4,member.ref()); memberInsert.text(5,utf8(member.role())); memberInsert.insert();
            ++result.stats.relationMembers;
        }
        ++result.stats.relations;
    }
};
QJsonObject summaryJson(const app::ImportResult& r) {
    QJsonArray warnings; for(const auto& w:r.warnings) warnings.append(w);
    return {{"name",r.fileName},{"sourcePath",r.filePath},{"format",r.sourceFormat},
        {"generator",r.sourceName},{"datasetId",r.datasetId},{"sha256",r.sourceSha256},
        {"importedAt",r.importedAt},{"bytes",QString::number(r.fileSize)},
        {"elapsedMs",QString::number(r.elapsedMs)},{"nodes",QString::number(r.stats.nodes)},
        {"ways",QString::number(r.stats.ways)},{"relations",QString::number(r.stats.relations)},
        {"tags",QString::number(r.stats.tags)},{"nodeReferences",QString::number(r.stats.nodeReferences)},
        {"relationMembers",QString::number(r.stats.relationMembers)},{"warnings",warnings},
        {"hasBounds",r.hasBounds},{"minLat",r.minLat},{"minLon",r.minLon},
        {"maxLat",r.maxLat},{"maxLon",r.maxLon},{"reader",QString("libosmium ")+LIBOSMIUM_VERSION_STRING}};
}
}
OsmImportParser::OsmImportParser(QString directory)
    : directory_(directory.isEmpty()?defaultImportCacheDirectory():std::move(directory)) {}
QString OsmImportParser::inputFormat(const QString& path) {
    const auto name=QFileInfo(path).fileName().toLower();
    if(name.endsWith(".osm.gz")) return "osm.gz";
    if(name.endsWith(".osm.bz2")) return "osm.bz2";
    if(name.endsWith(".pbf")) return "pbf";
    if(name.endsWith(".osm")) return "osm";
    return {};
}
app::ImportResult OsmImportParser::parse(const QString& path, const app::ImportProgressCallback& progress,
        const app::ImportLogCallback& log, const app::ImportCancelCallback& cancelled) const {
    app::ImportResult result;
    QElapsedTimer timer; timer.start();
    const QFileInfo before(path);
    result.filePath=before.absoluteFilePath(); result.fileName=before.fileName();
    result.fileSize=before.size(); result.sourceFormat=inputFormat(path); result.formatVersion=1;
    auto checkCancel=[&] { if(cancelled && cancelled()) throw Cancelled{}; };
    auto report=[&](int percent,const QString& stage) { if(progress) progress(percent,stage); };
    auto message=[&](const QString& text) { if(log) log(text); };
    try {
        checkCancel();
        if(result.sourceFormat.isEmpty()) throw std::runtime_error("Unsupported format: use .osm, .pbf, .osm.gz or .osm.bz2");
        if(!before.isFile() || result.fileSize<=0) throw std::runtime_error("Input is not a non-empty regular file");
        QFile input(result.filePath);
        if(!input.open(QIODevice::ReadOnly)) throw std::runtime_error(input.errorString().toStdString());
        report(0,"校验源文件");
        QCryptographicHash hash(QCryptographicHash::Sha256);
        while(!input.atEnd()) {
            checkCancel();
            const auto block=input.read(1024*1024);
            if(block.isEmpty() && input.error()!=QFileDevice::NoError)
                throw std::runtime_error(input.errorString().toStdString());
            hash.addData(block);
            report(int(input.pos()*10/result.fileSize),"校验源文件");
        }
        result.sourceSha256=QString::fromLatin1(hash.result().toHex()); input.close();
        if(!QDir().mkpath(directory_)) throw std::runtime_error("Cannot create import cache directory");
        QTemporaryDir staging(QDir(directory_).filePath(".import-XXXXXX"));
        if(!staging.isValid()) throw std::runtime_error("Cannot create import staging directory");
        result.datasetId=QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString temporary=staging.filePath("data.sqlite");
        const QString finalPath=QDir(directory_).absoluteFilePath(result.datasetId+".sqlite");
        {
            Db db(temporary);
            sqlite3_progress_handler(db.value,10000,[](void* arg)->int {
                const auto* callback=static_cast<const app::ImportCancelCallback*>(arg);
                return *callback && (*callback)() ? 1 : 0;
            },const_cast<app::ImportCancelCallback*>(&cancelled));
            db.exec("PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA cache_size=-8192; BEGIN IMMEDIATE;"
                "CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL) WITHOUT ROWID;"
                "CREATE TABLE nodes(id INTEGER PRIMARY KEY,lat REAL NOT NULL,lon REAL NOT NULL,tags TEXT NOT NULL);"
                "CREATE TABLE ways(id INTEGER PRIMARY KEY,tags TEXT NOT NULL);"
                "CREATE TABLE way_nodes(way_id INTEGER,seq INTEGER,node_id INTEGER NOT NULL,PRIMARY KEY(way_id,seq)) WITHOUT ROWID;"
                "CREATE TABLE relations(id INTEGER PRIMARY KEY,tags TEXT NOT NULL);"
                "CREATE TABLE relation_members(relation_id INTEGER,seq INTEGER,type INTEGER,member_id INTEGER,role TEXT,PRIMARY KEY(relation_id,seq)) WITHOUT ROWID;");
            message("使用 libosmium " LIBOSMIUM_VERSION_STRING " 流式读取；缓存为单事务 SQLite，失败/取消不发布。");
            const auto fileBytes=result.filePath.toUtf8(); const auto formatBytes=result.sourceFormat.toUtf8();
            osmium::io::Reader reader(osmium::io::File(fileBytes.constData(),formatBytes.constData()));
            const auto header=reader.header();
            if(header.has_multiple_object_versions()) throw std::runtime_error("OSM history/change files are not supported; supply a snapshot");
            result.sourceName=QString::fromStdString(header.get("generator"));
            for(const auto& box:header.boxes()) if(!box.valid()) throw std::runtime_error("Invalid OSM header bounds");
            quint64 calls=0; QElapsedTimer last; last.start();
            auto checkpoint=[&] {
                checkCancel();
                if(++calls%1000==0 && last.elapsed()>=100) {
                    report(qMin(85,10+int(reader.offset()*75/result.fileSize)),"解析并保存节点、道路和关系");
                    last.restart();
                }
            };
            report(10,"解析并保存节点、道路和关系");
            {
                Handler handler(db,result,checkpoint);
                osmium::apply(reader,handler);
            }
            reader.close(); checkCancel();
            if(result.stats.nodes+result.stats.ways+result.stats.relations==0)
                throw std::runtime_error("OSM input contains no nodes, ways or relations");
            report(90,"核查引用完整性");
            auto missing=[&](const char* sql,const QString& label) {
                Statement s(db,sql); if(s.row() && s.integer(0)>0)
                    result.warnings.append(QString("%1：%2 个引用缺少对象；原始引用已保留，导航可用性尚未验证。").arg(label).arg(s.integer(0)));
            };
            missing("SELECT count(*) FROM way_nodes r LEFT JOIN nodes n ON n.id=r.node_id WHERE n.id IS NULL","道路节点");
            missing("SELECT count(*) FROM relation_members r LEFT JOIN nodes n ON n.id=r.member_id WHERE r.type=0 AND n.id IS NULL","关系节点");
            missing("SELECT count(*) FROM relation_members r LEFT JOIN ways w ON w.id=r.member_id WHERE r.type=1 AND w.id IS NULL","关系道路");
            missing("SELECT count(*) FROM relation_members r LEFT JOIN relations x ON x.id=r.member_id WHERE r.type=2 AND x.id IS NULL","关系成员");
            checkCancel();
            const QFileInfo after(result.filePath);
            if(after.size()!=before.size() || after.lastModified()!=before.lastModified())
                throw std::runtime_error("Source file changed during import; cache was not published");
            result.importedAt=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
            result.elapsedMs=timer.elapsed();
            Statement meta(db,"INSERT INTO metadata VALUES(?,?)");
            meta.text(1,"schema"); meta.text(2,"1"); meta.insert();
            meta.text(1,"summary"); meta.text(2,QString::fromUtf8(QJsonDocument(summaryJson(result)).toJson(QJsonDocument::Compact))); meta.insert();
            report(99,"提交导入缓存"); checkCancel(); db.exec("COMMIT;");
        }
        checkCancel();
        if(!QFile::rename(temporary,finalPath)) throw std::runtime_error("Cannot publish completed cache atomically");
        result.cachePath=finalPath; result.ok=true;
        report(100,"缓存已保存");
        message(QString("已保存：节点 %1，道路 %2，关系 %3；%4").arg(result.stats.nodes).arg(result.stats.ways).arg(result.stats.relations).arg(finalPath));
        for(const auto& warning:result.warnings) message(warning);
    } catch(const Cancelled&) {
        result.cancelled=true; result.error="导入已取消；未发布缓存。";
    } catch(const std::exception& error) {
        result.cancelled=cancelled && cancelled();
        result.error=result.cancelled?QString("导入已取消；未发布缓存。"):QString::fromUtf8(error.what());
        message(result.error);
    }
    return result;
}
}
