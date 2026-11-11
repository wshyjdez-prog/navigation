#include "contracts/MapGeometry.h"
#include "contracts/ImportedDataset.h"
#include "FeatureRules.h"
#include "Sqlite.h"
#include <QHash>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <optional>
namespace p1::data {
namespace {
using TagMap=detail::TagMap;
using detail::classifyArea;
struct Chain { QVector<qint64> ids; QVector<QPointF> points; };
struct Member { qint64 id; QString role; int type; };
bool valid(const Chain& c){
 if(c.ids.size()<4||c.ids.front()!=c.ids.back()||c.points.size()!=c.ids.size())return false;
 double area=0;const auto origin=c.points.front();QSet<qint64> unique;
 for(int i=0;i<c.points.size()-1;++i){const auto p=c.points[i]-origin,q=c.points[i+1]-origin;
  if(!std::isfinite(p.x())||!std::isfinite(p.y()))return false;
  if(unique.contains(c.ids[i]))return false;unique.insert(c.ids[i]);area+=p.x()*q.y()-q.x()*p.y();}
 return unique.size()>=3&&std::abs(area)>1e-16;
}
bool inside(QPointF p,const QVector<QPointF>& ring){bool in=false;for(int i=0,j=ring.size()-1;i<ring.size();j=i++){const auto a=ring[i],b=ring[j];if((a.y()>p.y())!=(b.y()>p.y())&&p.x()<(b.x()-a.x())*(p.y()-a.y())/(b.y()-a.y())+a.x())in=!in;}return in;}
// Endpoint IDs, rather than rounded coordinates, determine connectivity.
bool assemble(QVector<Chain> parts,QVector<Chain>& rings,const std::function<void()>& check){
 QHash<qint64,QVector<int>> ends;QVector<bool> used(parts.size(),false);
 for(int i=0;i<parts.size();++i){if(parts[i].ids.size()<2)return false;if(parts[i].ids.front()!=parts[i].ids.back()){ends[parts[i].ids.front()].append(i);ends[parts[i].ids.back()].append(i);}}
 for(auto it=ends.cbegin();it!=ends.cend();++it)if(it.value().size()!=2)return false;
 for(int i=0;i<parts.size();++i){check();if(used[i])continue;used[i]=true;auto ring=std::move(parts[i]);
  while(ring.ids.back()!=ring.ids.front()){check();int next=-1;for(auto index:ends.value(ring.ids.back()))if(!used[index]){next=index;break;}if(next<0)return false;used[next]=true;auto& p=parts[next];
   if(p.ids.front()!=ring.ids.back()){std::reverse(p.ids.begin(),p.ids.end());std::reverse(p.points.begin(),p.points.end());}
   for(int j=1;j<p.ids.size();++j){ring.ids.append(p.ids[j]);ring.points.append(p.points[j]);}}
  if(!valid(ring))return false;rings.append(std::move(ring));}
 return !rings.isEmpty();
}
}
AreaReadStats visitMapAreas(const app::ImportResult& entry,std::shared_ptr<const OsmxDataset> ds,const std::function<void(const MapArea&)>& emitArea,const std::shared_ptr<std::atomic_bool>& cancel){
 AreaReadStats stats;auto check=[&]{if(cancel&&cancel->load())throw std::runtime_error("已取消面要素索引");};
 if(entry.sourceFormat!="osmx"){
  // Standard OSM: rings were assembled by libosmium at import time.
  detail::Db db(entry.cachePath,true);
  if(cancel)sqlite3_progress_handler(db.value,2000,[](void*p){return static_cast<std::atomic_bool*>(p)->load()?1:0;},cancel.get());
  detail::Statement rows(db,"SELECT kind,name,min_zoom,geometry FROM areas ORDER BY id");
  while(rows.row()){check();MapArea a;const auto kind=rows.integer(0);
   if(kind<int(MapAreaKind::Landuse)||kind>int(MapAreaKind::Building))throw std::runtime_error("导入缓存面类型损坏");
   a.kind=MapAreaKind(kind);a.name=rows.text(1);a.minZoom=int(rows.integer(2));detail::unpack(rows.blob(3),a.outer,a.holes);
   if(a.outer.size()<4)throw std::runtime_error("导入缓存几何损坏");
   emitArea(a);++stats.emitted;}
  stats.skipped=ImportedDataset(entry.cachePath).summary().stats.areasSkipped;
  return stats;
 }
 // OSMX keeps raw ways/relations in memory; assemble its rings here.
 if(!ds)throw std::runtime_error("OSMX 地图未加载");
 auto interned=[&](const QVector<OsmxTag>& values){TagMap t;for(auto v:values)t.insert(ds->stringAt(v.key),ds->stringAt(v.value));return t;};
 auto way=[&](qint64 id)->Chain{check();Chain c;auto wi=ds->wayIndex(id);if(!wi)return c;for(auto ni:ds->wayNodeIndices(*wi)){const auto& n=ds->nodeAt(ni);c.ids.append(n.id);c.points.append({n.lon,n.lat});}return c;};
 QSet<qint64> relationWays;
 auto relation=[&](const TagMap& tags,const QVector<Member>& members){check();if(tags.value("type")!="multipolygon"||tags.value("area")=="no")return;auto kind=classifyArea(tags);
  // Older OSM areas may place their thematic tags on outer member ways.
  if(!kind){for(const auto&m:members)if(m.type==1&&(m.role=="outer"||m.role.isEmpty())){TagMap t;auto wi=ds->wayIndex(m.id);if(wi)t=interned(ds->wayTags(*wi));auto candidate=classifyArea(t);if(candidate){if(kind&&kind->kind!=candidate->kind){++stats.skipped;return;}kind=candidate;}}}
  if(!kind)return;
  QVector<Chain> outerParts,innerParts;bool ok=true;for(const auto&m:members){check();if(m.type==1&&(m.role.isEmpty()||m.role=="outer"||m.role=="inner")){relationWays.insert(m.id);auto c=way(m.id);if(c.ids.isEmpty())ok=false;else(m.role=="inner"?innerParts:outerParts).append(std::move(c));}else if(m.role=="outer"||m.role=="inner")ok=false;}
  QVector<Chain> outers,inners;if(!ok||!assemble(std::move(outerParts),outers,check)||(!innerParts.isEmpty()&&!assemble(std::move(innerParts),inners,check))){++stats.skipped;return;}
  QVector<MapArea> areas;for(const auto&o:outers){auto a=*kind;a.outer=o.points;areas.append(std::move(a));}
  for(const auto& hole:inners){int owner=-1;double smallest=1e100;for(int i=0;i<areas.size();++i){bool contains=true;for(int j=0;j<hole.points.size()-1;++j)if(!inside(hole.points[j],areas[i].outer)){contains=false;break;}if(contains){double area=0;const auto origin=areas[i].outer.front();for(int j=1;j<areas[i].outer.size();++j){auto p=areas[i].outer[j-1]-origin,q=areas[i].outer[j]-origin;area+=p.x()*q.y()-q.x()*p.y();}if(std::abs(area)<smallest){smallest=std::abs(area);owner=i;}}}
   if(owner<0){++stats.skipped;return;}areas[owner].holes.append(hole.points);}
  for(const auto&a:areas){check();emitArea(a);++stats.emitted;}
 };
 for(quint32 i=0;i<ds->relationCount();++i){check();QVector<Member> members;for(const auto&m:ds->relationMembers(i))members.append({m.ref,ds->stringAt(m.role),int(m.type)});relation(interned(ds->relationTags(i)),members);}
 auto single=[&](qint64 id,const TagMap& tags){check();if(relationWays.contains(id))return;auto a=classifyArea(tags);if(!a)return;auto c=way(id);if(!valid(c)){++stats.skipped;return;}a->outer=std::move(c.points);emitArea(*a);++stats.emitted;};
 for(quint32 i=0;i<ds->wayCount();++i)single(ds->wayAt(i).id,interned(ds->wayTags(i)));
 return stats;
}
}
