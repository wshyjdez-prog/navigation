#include "contracts/ImportedDataset.h"
#include "data/osmx/OsmxParser.h"
#include "Sqlite.h"
#include <QJsonArray>
#include <QJsonDocument>
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
    // This bounded query deliberately uses the existing cache schema. It does
    // not modify the source or introduce another spatial-index implementation.
    detail::Statement candidates(db,
        "SELECT DISTINCT way_id FROM way_nodes WHERE node_id IN "
        "(SELECT id FROM nodes WHERE lat BETWEEN ? AND ? AND lon BETWEEN ? AND ?) ORDER BY way_id");
    candidates.real(1,b.minLat); candidates.real(2,b.maxLat);
    candidates.real(3,b.minLon); candidates.real(4,b.maxLon);
    QVector<qint64> ids;
    while (candidates.row()) { check(); ids.append(candidates.integer(0));
        if (ids.size()>100000) throw std::runtime_error("区域过大，请缩小工作半径"); }
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
    for (qint64 id:ids) {
        check();
        detail::Statement w(db,"SELECT tags FROM ways WHERE id=?"); w.integer(1,id);
        if (!w.row()) throw std::runtime_error("损坏的道路缓存");
        QJsonParseError parseError;
        const auto json=QJsonDocument::fromJson(w.text(0).toUtf8(), &parseError);
        if(parseError.error!=QJsonParseError::NoError || !json.isArray())
            throw std::runtime_error("道路标签损坏");
        const auto tags=json.array(); bool highway=false;
        for(const auto& t:tags) { const auto pair=t.toArray(); if(!pair.isEmpty()&&pair.at(0).toString()=="highway") highway=true; }
        if(!highway) continue;
        struct Ref { qint64 id; double lat,lon; };
        QVector<Ref> refs; bool missing=false;
        detail::Statement r(db,"SELECT wn.node_id,n.lat,n.lon,n.id FROM way_nodes wn "
            "LEFT JOIN nodes n ON n.id=wn.node_id WHERE wn.way_id=? ORDER BY wn.seq");
        r.integer(1,id);
        while(r.row()) {
            if(sqlite3_column_type(r.value,3)==SQLITE_NULL) { missing=true; break; }
            const double lat=r.real(1),lon=r.real(2);
            if(!std::isfinite(lat)||!std::isfinite(lon)||std::abs(lat)>90||std::abs(lon)>180)
                throw std::runtime_error("非法道路坐标");
            refs.append({r.integer(0),lat,lon});
        }
        if(missing || refs.size()<2) { d.info.warnings.append(QString("道路 %1 引用不完整，已跳过").arg(id)); continue; }
        OsmxWay way; way.id=id; way.refFirst=quint32(d.wayNodes.size());
        way.refCount=quint32(refs.size()); way.tagFirst=quint32(d.wayTags.size());
        for(const auto& t:tags) { const auto a=t.toArray();
            if(a.size()!=2 || !a[0].isString() || !a[1].isString()) throw std::runtime_error("道路标签损坏");
            d.wayTags.append({intern(a[0].toString()),intern(a[1].toString())}); }
        way.tagCount=quint32(tags.size());
        fillRoadAttributes(d.wayTags,way.tagFirst,way.tagCount,d.strings,way.refCount,way.attributes);
        for(const auto& ref:refs) { auto it=nodes.constFind(ref.id); quint32 n;
            if(it==nodes.cend()) { n=quint32(d.nodes.size()); nodes.insert(ref.id,n);
                d.nodes.append({ref.id,ref.lat,ref.lon,0,0}); }
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
