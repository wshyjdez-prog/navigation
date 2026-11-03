#include "contracts/MapGeometry.h"
#include "Sqlite.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <cmath>
namespace p1::data {
namespace {
int zoom(RoadClass c) {
    if(c==RoadClass::Motorway||c==RoadClass::MotorwayLink)return 0;
    if(c==RoadClass::Trunk||c==RoadClass::TrunkLink)return 5;
    if(c==RoadClass::Primary||c==RoadClass::PrimaryLink)return 8;
    if(c==RoadClass::Secondary||c==RoadClass::SecondaryLink)return 10;
    if(c==RoadClass::Tertiary||c==RoadClass::TertiaryLink)return 11;
    if(c==RoadClass::Residential||c==RoadClass::Unclassified||c==RoadClass::LivingStreet)return 12;
    return 15;
}
}
void visitMapRoads(const app::ImportResult& entry,std::shared_ptr<const OsmxDataset> ds,
    const std::function<void(const MapRoad&)>& visit,const std::shared_ptr<std::atomic_bool>& cancel) {
    auto check=[&]{if(cancel&&cancel->load())throw std::runtime_error("已取消地图索引构建");};
    if(entry.sourceFormat=="osmx") {
        if(!ds)throw std::runtime_error("OSMX 地图未加载");
        for(quint32 i=0;i<ds->wayCount();++i){check();const auto& w=ds->wayAt(i);if(!ds->wayHasTag(i,u"highway")&&w.attributes.roadClass==RoadClass::Unknown)continue;MapRoad r;r.name=ds->displayName(i);r.minZoom=zoom(w.attributes.roadClass);
            for(auto ni:ds->wayNodeIndices(i)){const auto& n=ds->nodeAt(ni);r.points.append({n.lon,n.lat});}
            if(r.points.size()>1)visit(r);
        }return;
    }
    detail::Db db(entry.cachePath,true);
    if(cancel)sqlite3_progress_handler(db.value,2000,[](void* p){return static_cast<std::atomic_bool*>(p)->load()?1:0;},cancel.get());
    detail::Statement ways(db,"SELECT id,tags FROM ways ORDER BY id");
    while(ways.row()) {
        check();QJsonParseError error;const auto tags=QJsonDocument::fromJson(ways.text(1).toUtf8(),&error);
        if(error.error!=QJsonParseError::NoError||!tags.isArray())throw std::runtime_error("地图道路标签损坏");
        QString highway,name,localName;
        for(const auto& t:tags.array()){auto pair=t.toArray();if(pair.size()!=2)throw std::runtime_error("地图标签格式错误");
            auto key=pair[0].toString(),value=pair[1].toString();if(key=="highway")highway=value;else if(key=="name")name=value;else if(key=="name:zh")localName=value;}
        if(highway.isEmpty())continue;
        MapRoad r;r.name=localName.isEmpty()?name:localName;r.minZoom=zoom(roadClassFromHighway(highway));
        detail::Statement refs(db,"SELECT n.lon,n.lat,n.id FROM way_nodes w LEFT JOIN nodes n ON n.id=w.node_id WHERE w.way_id=? ORDER BY w.seq");refs.integer(1,ways.integer(0));
        bool missing=false;while(refs.row()){check();if(sqlite3_column_type(refs.value,2)==SQLITE_NULL){missing=true;break;}
            const double x=refs.real(0),y=refs.real(1);if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>180||std::abs(y)>90)throw std::runtime_error("地图包含无效坐标");r.points.append({x,y});}
        if(!missing&&r.points.size()>1)visit(r);
    }
}
}
