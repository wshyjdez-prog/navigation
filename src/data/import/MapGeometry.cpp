#include "contracts/MapGeometry.h"
#include "FeatureRules.h"
#include "Sqlite.h"
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
    detail::Statement roads(db,"SELECT highway,name,coords FROM roads ORDER BY id");
    while(roads.row()) {
        check();MapRoad r;r.name=roads.text(1);r.minZoom=zoom(roadClassFromHighway(roads.text(0)));
        detail::unpack(roads.blob(2),r.points);
        for(const auto& p:r.points)if(!std::isfinite(p.x())||!std::isfinite(p.y())||std::abs(p.x())>180||std::abs(p.y())>90)throw std::runtime_error("地图包含无效坐标");
        if(r.points.size()>1)visit(r);
    }
}
}
