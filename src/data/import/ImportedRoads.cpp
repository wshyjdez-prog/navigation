#include "contracts/ImportedDataset.h"
#include "data/osmx/OsmxParser.h"
#include "FeatureRules.h"
#include "Sqlite.h"
#include <QHash>
#include <cmath>
#include <stdexcept>

namespace p1::data {
std::shared_ptr<const OsmxDataset> ImportedDataset::roads(const OsmxBounds& b,
        const std::shared_ptr<std::atomic_bool>& cancelled, bool allowEmpty) const {
    if (!b.valid || !std::isfinite(b.minLat) || !std::isfinite(b.maxLat) ||
        !std::isfinite(b.minLon) || !std::isfinite(b.maxLon) || b.minLat < -90 || b.maxLat > 90 ||
        b.minLon < -180 || b.maxLon > 180 || b.minLat >= b.maxLat || b.minLon >= b.maxLon ||
        b.maxLat-b.minLat > 0.3 || b.maxLon-b.minLon > 0.5)
        throw std::invalid_argument("工作区域无效或过大，请选择半径 0.2–10 km 的区域");
    auto check = [&] { if (cancelled && cancelled->load()) throw std::runtime_error("已取消"); };
    check();
    const auto meta = summary();
    detail::Db db(path_, true);
    if (cancelled) sqlite3_progress_handler(db.value, 2000, [](void* p) {
        return static_cast<std::atomic_bool*>(p)->load() ? 1 : 0;
    }, cancelled.get());
    // Bounding boxes stored with each road narrow the scan; the exact rule
    // (at least one node inside the rectangle) is applied to the geometry.
    detail::Statement roads(db,
        "SELECT id,tags,refs,coords FROM roads WHERE max_lat>=? AND min_lat<=? AND max_lon>=? AND min_lon<=? ORDER BY id");
    roads.real(1,b.minLat); roads.real(2,b.maxLat); roads.real(3,b.minLon); roads.real(4,b.maxLon);
    OsmxDataset::Data d;
    d.info.datasetId = meta.datasetId;
    d.info.sourceFileName = meta.fileName; d.info.sourcePath = meta.filePath;
    d.info.sourceSha256 = meta.sourceSha256; d.info.importedAt = meta.importedAt;
    d.info.formatVersion = 1; d.info.fileBytes = meta.fileSize;
    QHash<qint64,quint32> nodes;
    QHash<QString,quint32> strings;
    auto intern = [&](const QString& s) { auto it=strings.constFind(s);
        if(it!=strings.cend()) return *it;
        const auto index=quint32(d.strings.size()); d.strings.append(s); strings.insert(s,index); return index; };
    int selected=0;
    while (roads.row()) {
        check();
        QVector<qint64> refs; QVector<QPointF> points;
        detail::unpack(roads.blob(2),refs); detail::unpack(roads.blob(3),points);
        if(refs.size()<2 || refs.size()!=points.size()) throw std::runtime_error("损坏的道路缓存");
        bool inside=false;
        for(const auto& p:points) {
            if(!std::isfinite(p.x())||!std::isfinite(p.y())||std::abs(p.y())>90||std::abs(p.x())>180)
                throw std::runtime_error("非法道路坐标");
            inside=inside||b.contains(p.y(),p.x());
        }
        if(!inside) continue;
        if(++selected>100000) throw std::runtime_error("区域过大，请缩小工作半径");
        const auto tags=detail::decodeTags(roads.text(1));
        OsmxWay way; way.id=roads.integer(0); way.refFirst=quint32(d.wayNodes.size());
        way.refCount=quint32(refs.size()); way.tagFirst=quint32(d.wayTags.size());
        for(const auto& t:tags) d.wayTags.append({intern(t.first),intern(t.second)});
        way.tagCount=quint32(tags.size());
        fillRoadAttributes(d.wayTags,way.tagFirst,way.tagCount,d.strings,way.refCount,way.attributes);
        for(int i=0;i<refs.size();++i) { auto it=nodes.constFind(refs[i]); quint32 n;
            if(it==nodes.cend()) { n=quint32(d.nodes.size()); nodes.insert(refs[i],n);
                d.nodes.append({refs[i],points[i].y(),points[i].x(),0,0}); }
            else n=*it;
            d.wayNodes.append(n);
        }
        d.ways.append(way);
        if(d.nodes.size()>250000 || d.wayNodes.size()>500000) throw std::runtime_error("区域路网过大，请缩小半径");
    }
    check();
    if(d.ways.isEmpty() && !allowEmpty) throw std::runtime_error("区域内没有道路，请调整中心或半径");
    d.info.wayTagCount=quint64(d.wayTags.size());
    d.info.wayNodeReferenceCount=quint64(d.wayNodes.size());
    OsmxError error; auto result=OsmxDataset::create(std::move(d), &error);
    if(!result) throw std::runtime_error(error.toString().toStdString());
    check(); return result;
}
}
