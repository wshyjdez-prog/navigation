#include "OsmImportParser.h"
#include "contracts/ImportedDataset.h"
#include "FeatureRules.h"
#include "Sqlite.h"
#include <osmium/area/assembler.hpp>
#include <osmium/area/multipolygon_manager.hpp>
#include <osmium/handler.hpp>
#include <osmium/handler/node_locations_for_ways.hpp>
#include <osmium/index/map/sparse_file_array.hpp>
#include <osmium/index/map/sparse_mem_array.hpp>
#include <osmium/io/any_input.hpp>
#include <osmium/tags/tags_filter.hpp>
#include <osmium/version.hpp>
#include <osmium/visitor.hpp>
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

// Standard OSM import built on libosmium.
//
// The source is read twice. Pass 1 only looks at relations so that
// osmium::area::MultipolygonManager knows which ways belong to multipolygons.
// Pass 2 streams everything through this handler chain:
//   Features (validation, statistics, POI nodes)
//   -> NodeLocationsForWays (attaches coordinates to way node references)
//   -> Features::way (roads)
//   -> MultipolygonManager (closed ways and multipolygons become osmium::Area,
//      rings assembled by osmium::area::Assembler) -> Features::area
// Only the three feature kinds are stored: POIs, roads and areas. Raw nodes,
// ways and relations are not kept, so the cache is no longer an OSM mirror.
namespace p1::data {
using namespace detail;
namespace {
struct Cancelled {};
enum OsmType { NodeType = 0, WayType = 1, RelationType = 2 };

using PositiveIndex = osmium::index::map::SparseFileArray<osmium::unsigned_object_id_type, osmium::Location>;
using NegativeIndex = osmium::index::map::SparseMemArray<osmium::unsigned_object_id_type, osmium::Location>;
using Locations = osmium::handler::NodeLocationsForWays<PositiveIndex, NegativeIndex>;
using AreaManager = osmium::area::MultipolygonManager<osmium::area::Assembler>;

QString utf8(const char* text) {
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString value = decoder(QByteArrayView(text));
    if (decoder.hasError()) throw std::runtime_error("OSM contains invalid UTF-8");
    return value;
}
Tags tagList(const osmium::TagList& tags) {
    Tags result; result.reserve(int(tags.size()));
    for (const auto& tag : tags) result.append(qMakePair(utf8(tag.key()), utf8(tag.value())));
    return result;
}
TagMap tagMap(const Tags& tags) {
    TagMap result; result.reserve(tags.size());
    for (const auto& tag : tags) result.insert(tag.first, tag.second);
    return result;
}
// Tags that can make a closed way or multipolygon interesting. The precise
// decision is taken later by classifyArea()/classifyPoi(); this filter only
// spares the assembler from building rings nobody will store.
osmium::TagsFilter areaFilter() {
    osmium::TagsFilter filter{false};
    for (const char* key : {"building", "building:part", "landuse", "natural", "waterway", "railway", "aeroway"})
        filter.add_rule(true, osmium::TagMatcher{key});
    for (const char* key : kPoiKeys) filter.add_rule(true, osmium::TagMatcher{key});
    return filter;
}

struct Features : osmium::handler::Handler {
    Statement poiInsert, roadInsert, areaInsert;
    app::ImportResult& result;
    std::function<void()> check;
    quint64 missingNodeRefs = 0, roadsSkipped = 0, areasSkipped = 0;
    osmium::item_type lastType = osmium::item_type::undefined;

    Features(Db& db, app::ImportResult& r, std::function<void()> callback)
        : poiInsert(db, "INSERT INTO pois(osm_type,osm_id,lat,lon,category,kind,name,tags) VALUES(?,?,?,?,?,?,?,?)"),
          roadInsert(db, "INSERT INTO roads VALUES(?,?,?,?,?,?,?,?,?,?)"),
          areaInsert(db, "INSERT INTO areas(osm_type,osm_id,kind,name,min_zoom,min_lon,min_lat,max_lon,max_lat,geometry) "
                         "VALUES(?,?,?,?,?,?,?,?,?,?)"),
          result(r), check(std::move(callback)) {}

    void object(const osmium::OSMObject& obj) {
        check();
        if (!obj.visible()) throw std::runtime_error("Deleted/history objects are not supported; supply an OSM snapshot");
        if (obj.id() == 0) throw std::runtime_error("OSM object ID must not be zero");
        // Coordinates are attached while streaming, so nodes must precede ways
        // and ways must precede relations (the order every OSM tool writes).
        if (obj.type() != lastType) {
            if (lastType != osmium::item_type::undefined && obj.type() < lastType)
                throw std::runtime_error("OSM input must list nodes, then ways, then relations");
            lastType = obj.type();
        }
        result.stats.tags += obj.tags().size();
    }
    void poi(int type, qint64 id, double lat, double lon, const Tags& tags, const TagMap& map) {
        const auto kind = classifyPoi(map);
        if (!kind) return;
        poiInsert.integer(1, type); poiInsert.integer(2, id); poiInsert.real(3, lat); poiInsert.real(4, lon);
        poiInsert.text(5, kind->category); poiInsert.text(6, kind->kind); poiInsert.text(7, displayName(map));
        poiInsert.text(8, encodeTags(tags)); poiInsert.insert();
        ++result.stats.pois;
    }
    void node(const osmium::Node& n) {
        object(n);
        if (!n.location().valid()) throw std::runtime_error("OSM node has no valid WGS84 location");
        const double lat = n.location().lat(), lon = n.location().lon();
        if (!std::isfinite(lat) || !std::isfinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180)
            throw std::runtime_error("OSM node coordinates are outside WGS84 bounds");
        if (!result.hasBounds) {
            result.minLat = result.maxLat = lat; result.minLon = result.maxLon = lon; result.hasBounds = true;
        } else {
            result.minLat = qMin(result.minLat, lat); result.maxLat = qMax(result.maxLat, lat);
            result.minLon = qMin(result.minLon, lon); result.maxLon = qMax(result.maxLon, lon);
        }
        ++result.stats.nodes;
        if (n.tags().empty()) return;
        const auto tags = tagList(n.tags());
        poi(NodeType, n.id(), lat, lon, tags, tagMap(tags));
    }
    // Runs after NodeLocationsForWays: node references already carry locations.
    void way(const osmium::Way& w) {
        object(w);
        ++result.stats.ways; result.stats.nodeReferences += w.nodes().size();
        quint64 missing = 0;
        for (const auto& ref : w.nodes()) if (!ref.location().valid()) ++missing;
        missingNodeRefs += missing;
        const char* highway = w.tags().get_value_by_key("highway");
        if (!highway || !*highway) return;
        // A road with a gap is rejected as a whole: no fabricated links.
        if (missing || w.nodes().size() < 2) { ++roadsSkipped; return; }
        const auto tags = tagList(w.tags());
        QVector<qint64> refs; QVector<QPointF> points;
        refs.reserve(int(w.nodes().size())); points.reserve(int(w.nodes().size()));
        double minLon = 180, minLat = 90, maxLon = -180, maxLat = -90;
        for (const auto& ref : w.nodes()) {
            const double lon = ref.location().lon(), lat = ref.location().lat();
            refs.append(ref.ref()); points.append({lon, lat});
            minLon = qMin(minLon, lon); maxLon = qMax(maxLon, lon); minLat = qMin(minLat, lat); maxLat = qMax(maxLat, lat);
        }
        roadInsert.integer(1, w.id()); roadInsert.text(2, utf8(highway)); roadInsert.text(3, displayName(tagMap(tags)));
        roadInsert.text(4, encodeTags(tags));
        roadInsert.real(5, minLon); roadInsert.real(6, minLat); roadInsert.real(7, maxLon); roadInsert.real(8, maxLat);
        roadInsert.blob(9, pack(refs)); roadInsert.blob(10, pack(points)); roadInsert.insert();
        ++result.stats.roads;
    }
    void relation(const osmium::Relation& r) {
        object(r);
        ++result.stats.relations; result.stats.relationMembers += r.members().size();
    }
    // Called with the polygons libosmium assembled. One cache row per outer
    // ring; its inner rings are the holes the assembler assigned to it.
    void area(const osmium::Area& a) {
        check();
        const auto tags = tagList(a.tags());
        const auto map = tagMap(tags);
        const auto theme = classifyArea(map);
        const int type = a.from_way() ? WayType : RelationType;
        const auto box = a.envelope();
        if (a.num_rings().first == 0 || !box.valid()) {   // assembler found no valid ring
            if (theme) ++areasSkipped;
            return;
        }
        poi(type, a.orig_id(), (box.bottom_left().lat() + box.top_right().lat()) / 2,
            (box.bottom_left().lon() + box.top_right().lon()) / 2, tags, map);
        if (!theme) return;
        auto points = [](const osmium::NodeRefList& ring) {
            QVector<QPointF> result; result.reserve(int(ring.size()));
            for (const auto& ref : ring) result.append({ref.lon(), ref.lat()});
            return result;
        };
        for (const auto& outer : a.outer_rings()) {
            check();
            const auto ring = points(outer);
            QVector<QVector<QPointF>> holes;
            for (const auto& inner : a.inner_rings(outer)) holes.append(points(inner));
            const auto bounds = outer.envelope();
            areaInsert.integer(1, type); areaInsert.integer(2, a.orig_id()); areaInsert.integer(3, int(theme->kind));
            areaInsert.text(4, theme->name); areaInsert.integer(5, theme->minZoom);
            areaInsert.real(6, bounds.bottom_left().lon()); areaInsert.real(7, bounds.bottom_left().lat());
            areaInsert.real(8, bounds.top_right().lon()); areaInsert.real(9, bounds.top_right().lat());
            areaInsert.blob(10, pack(ring, holes)); areaInsert.insert();
            ++result.stats.areas;
        }
    }
};
// NodeLocationsForWays silently keeps the last location of a repeated node ID.
// A snapshot must not contain one, so reject it once the index is sorted.
template <typename Index> void rejectDuplicateNodes(Index& index, const std::function<void()>& check) {
    index.sort();
    quint64 count = 0; bool first = true; osmium::unsigned_object_id_type previous = 0;
    for (const auto& entry : index) {
        if ((++count & 0xffff) == 0) check();
        if (!first && entry.first == previous) throw std::runtime_error("OSM input contains a duplicate node ID");
        previous = entry.first; first = false;
    }
}
QJsonObject summaryJson(const app::ImportResult& r) {
    QJsonArray warnings; for(const auto& w:r.warnings) warnings.append(w);
    return {{"name",r.fileName},{"sourcePath",r.filePath},{"format",r.sourceFormat},
        {"generator",r.sourceName},{"datasetId",r.datasetId},{"sha256",r.sourceSha256},
        {"importedAt",r.importedAt},{"bytes",QString::number(r.fileSize)},
        {"elapsedMs",QString::number(r.elapsedMs)},{"nodes",QString::number(r.stats.nodes)},
        {"ways",QString::number(r.stats.ways)},{"relations",QString::number(r.stats.relations)},
        {"tags",QString::number(r.stats.tags)},{"nodeReferences",QString::number(r.stats.nodeReferences)},
        {"relationMembers",QString::number(r.stats.relationMembers)},
        {"pois",QString::number(r.stats.pois)},{"roads",QString::number(r.stats.roads)},
        {"areas",QString::number(r.stats.areas)},{"areasSkipped",QString::number(r.stats.areasSkipped)},
        {"warnings",warnings},
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
    result.fileSize=before.size(); result.sourceFormat=inputFormat(path); result.formatVersion=2;
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
                "CREATE TABLE pois(id INTEGER PRIMARY KEY,osm_type INTEGER NOT NULL,osm_id INTEGER NOT NULL,"
                " lat REAL NOT NULL,lon REAL NOT NULL,category TEXT NOT NULL,kind TEXT NOT NULL,name TEXT NOT NULL,tags TEXT NOT NULL);"
                "CREATE TABLE roads(id INTEGER PRIMARY KEY,highway TEXT NOT NULL,name TEXT NOT NULL,tags TEXT NOT NULL,"
                " min_lon REAL NOT NULL,min_lat REAL NOT NULL,max_lon REAL NOT NULL,max_lat REAL NOT NULL,"
                " refs BLOB NOT NULL,coords BLOB NOT NULL);"
                "CREATE TABLE areas(id INTEGER PRIMARY KEY,osm_type INTEGER NOT NULL,osm_id INTEGER NOT NULL,"
                " kind INTEGER NOT NULL,name TEXT NOT NULL,min_zoom INTEGER NOT NULL,"
                " min_lon REAL NOT NULL,min_lat REAL NOT NULL,max_lon REAL NOT NULL,max_lat REAL NOT NULL,geometry BLOB NOT NULL);");
            message("使用 libosmium " LIBOSMIUM_VERSION_STRING " 两遍读取：提取 POI、道路与面（面由 libosmium 组装）；"
                    "缓存为单事务 SQLite，失败/取消不发布。");
            const auto fileBytes=result.filePath.toUtf8(); const auto formatBytes=result.sourceFormat.toUtf8();
            const osmium::io::File file(fileBytes.constData(),formatBytes.constData());

            // Pass 1: relations only, to learn the multipolygon member ways.
            report(10,"扫描面关系");
            AreaManager areas{osmium::area::Assembler::config_type{},areaFilter()};
            {
                osmium::io::Reader reader(file,osmium::osm_entity_bits::relation);
                const auto header=reader.header();
                if(header.has_multiple_object_versions()) throw std::runtime_error("OSM history/change files are not supported; supply a snapshot");
                result.sourceName=QString::fromStdString(header.get("generator"));
                for(const auto& box:header.boxes()) if(!box.valid()) throw std::runtime_error("Invalid OSM header bounds");
                while(auto buffer=reader.read()) {
                    checkCancel(); osmium::apply(buffer,areas);
                    report(qMin(20,10+int(reader.offset()*10/result.fileSize)),"扫描面关系");
                }
                reader.close();
            }
            areas.prepare_for_lookup();
            checkCancel();

            // Pass 2: node locations live in a memory-mapped file next to the
            // cache, so a large extract does not need its nodes in RAM.
            QFile locationFile(staging.filePath("node-locations.bin"));
            if(!locationFile.open(QIODevice::ReadWrite)) throw std::runtime_error("Cannot create node location index");
            PositiveIndex positive(locationFile.handle()); NegativeIndex negative;
            Locations locations(positive,negative); locations.ignore_errors();
            report(20,"解析并保存 POI、道路和面");
            Features features(db,result,checkCancel);
            {
                osmium::io::Reader reader(file);
                quint64 buffers=0; QElapsedTimer last; last.start();
                auto assembled=areas.handler([&](osmium::memory::Buffer&& buffer) { osmium::apply(buffer,features); });
                // Features is listed twice on purpose: nodes are validated before
                // they enter the location index, ways are read after it.
                struct NodesOnly : osmium::handler::Handler { Features& f; explicit NodesOnly(Features& x):f(x){}
                    void node(const osmium::Node& n){f.node(n);} } nodes(features);
                struct WaysAndRelations : osmium::handler::Handler { Features& f; explicit WaysAndRelations(Features& x):f(x){}
                    void way(const osmium::Way& w){f.way(w);} void relation(const osmium::Relation& r){f.relation(r);} } rest(features);
                while(auto buffer=reader.read()) {
                    checkCancel();
                    osmium::apply(buffer,nodes,locations,rest,assembled);
                    if(++buffers%8==0 && last.elapsed()>=100) {
                        report(qMin(88,20+int(reader.offset()*68/result.fileSize)),"解析并保存 POI、道路和面");
                        last.restart();
                    }
                }
                assembled.flush();
                reader.close();
            }
            checkCancel();
            if(result.stats.nodes+result.stats.ways+result.stats.relations==0)
                throw std::runtime_error("OSM input contains no nodes, ways or relations");
            report(90,"核查数据完整性");
            rejectDuplicateNodes(positive,checkCancel); rejectDuplicateNodes(negative,checkCancel);
            quint64 incomplete=0;
            areas.for_each_incomplete_relation([&](const osmium::relations::RelationHandle&) { ++incomplete; });
            result.stats.areasSkipped=features.areasSkipped+incomplete;
            if(features.missingNodeRefs>0)
                result.warnings.append(QString("道路节点：%1 个引用缺少对象；受影响的 %2 条道路及相关面已跳过，不补造连接。")
                    .arg(features.missingNodeRefs).arg(features.roadsSkipped));
            if(incomplete>0)
                result.warnings.append(QString("面关系：%1 个 multipolygon 的成员不完整，已跳过。").arg(incomplete));
            if(features.areasSkipped>0)
                result.warnings.append(QString("面要素：%1 个多边形无法组装为有效环，已跳过。").arg(features.areasSkipped));
            db.exec("CREATE INDEX roads_bounds ON roads(min_lat,max_lat,min_lon,max_lon);"
                    "CREATE INDEX pois_position ON pois(lat,lon);"
                    "CREATE INDEX pois_category ON pois(category);");
            checkCancel();
            const QFileInfo after(result.filePath);
            if(after.size()!=before.size() || after.lastModified()!=before.lastModified())
                throw std::runtime_error("Source file changed during import; cache was not published");
            result.importedAt=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
            result.elapsedMs=timer.elapsed();
            Statement meta(db,"INSERT INTO metadata VALUES(?,?)");
            meta.text(1,"schema"); meta.text(2,kCacheSchema); meta.insert();
            meta.text(1,"summary"); meta.text(2,QString::fromUtf8(QJsonDocument(summaryJson(result)).toJson(QJsonDocument::Compact))); meta.insert();
            report(99,"提交导入缓存"); checkCancel(); db.exec("COMMIT;");
        }
        checkCancel();
        if(!QFile::rename(temporary,finalPath)) throw std::runtime_error("Cannot publish completed cache atomically");
        result.cachePath=finalPath; result.ok=true;
        report(100,"缓存已保存");
        message(QString("已保存：POI %1，道路 %2，面 %3（源数据节点 %4 / 路 %5 / 关系 %6）；%7")
            .arg(result.stats.pois).arg(result.stats.roads).arg(result.stats.areas)
            .arg(result.stats.nodes).arg(result.stats.ways).arg(result.stats.relations).arg(finalPath));
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
