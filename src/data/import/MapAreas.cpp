#include "contracts/MapGeometry.h"
#include "Sqlite.h"
#include <QHash>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <optional>
namespace p1::data {
namespace {
using Tags=QHash<QString,QString>;
struct Chain { QVector<qint64> ids; QVector<QPointF> points; };
struct Member { qint64 id; QString role; int type; };
Tags decode(const QString& text){Tags t;QJsonParseError e;auto doc=QJsonDocument::fromJson(text.toUtf8(),&e);if(e.error!=QJsonParseError::NoError||!doc.isArray())throw std::runtime_error("面要素标签损坏");for(auto v:doc.array()){auto a=v.toArray();if(a.size()!=2)throw std::runtime_error("面要素标签损坏");t.insert(a[0].toString(),a[1].toString());}return t;}
std::optional<MapArea> classify(const Tags& t){
 if(t.value("area")=="no")return {};
 MapArea a;a.name=t.value("name:zh",t.value("name"));
 auto yes=[&](const char* k){return t.contains(k)&&t.value(k)!="no"&& !t.value(k).isEmpty();};
 const auto land=t.value("landuse"),natural=t.value("natural"),leisure=t.value("leisure");
 if(yes("building")||yes("building:part")){a.kind=MapAreaKind::Building;a.minZoom=15;}
 else if(natural=="water"||t.value("waterway")=="riverbank"||land=="reservoir"||land=="basin"){a.kind=MapAreaKind::Water;a.minZoom=8;}
 else if(natural=="wood"||natural=="scrub"||natural=="grassland"||natural=="heath"||natural=="wetland"||land=="forest"||land=="grass"||land=="meadow"||land=="recreation_ground"||land=="village_green"||leisure=="park"||leisure=="garden"||leisure=="golf_course"){a.kind=MapAreaKind::Green;a.minZoom=10;}
 else if(yes("landuse")){a.kind=MapAreaKind::Landuse;a.minZoom=11;}
 else return {};
 return a;
}
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
 const bool osmx=entry.sourceFormat=="osmx";if(osmx&&!ds)throw std::runtime_error("OSMX 地图未加载");
 std::unique_ptr<detail::Db> db;if(!osmx){db=std::make_unique<detail::Db>(entry.cachePath,true);if(cancel)sqlite3_progress_handler(db->value,2000,[](void*p){return static_cast<std::atomic_bool*>(p)->load()?1:0;},cancel.get());}
 auto interned=[&](const QVector<OsmxTag>& values){Tags t;for(auto v:values)t.insert(ds->stringAt(v.key),ds->stringAt(v.value));return t;};
 auto way=[&](qint64 id)->Chain{check();Chain c;if(osmx){auto wi=ds->wayIndex(id);if(!wi)return c;for(auto ni:ds->wayNodeIndices(*wi)){const auto& n=ds->nodeAt(ni);c.ids.append(n.id);c.points.append({n.lon,n.lat});}}
  else{detail::Statement s(*db,"SELECT w.node_id,n.lon,n.lat,n.id FROM way_nodes w LEFT JOIN nodes n ON n.id=w.node_id WHERE w.way_id=? ORDER BY w.seq");s.integer(1,id);while(s.row()){check();if(sqlite3_column_type(s.value,3)==SQLITE_NULL)return {};auto x=s.real(1),y=s.real(2);if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>180||std::abs(y)>90)return {};c.ids.append(s.integer(0));c.points.append({x,y});}}return c;};
 QSet<qint64> relationWays;
 auto relation=[&](const Tags& tags,const QVector<Member>& members){check();if(tags.value("type")!="multipolygon"||tags.value("area")=="no")return;auto kind=classify(tags);
  // Older OSM areas may place their thematic tags on outer member ways.
  if(!kind){for(const auto&m:members)if(m.type==1&&(m.role=="outer"||m.role.isEmpty())){Tags t;if(osmx){auto wi=ds->wayIndex(m.id);if(wi)t=interned(ds->wayTags(*wi));}else{detail::Statement s(*db,"SELECT tags FROM ways WHERE id=?");s.integer(1,m.id);if(s.row())t=decode(s.text(0));}auto candidate=classify(t);if(candidate){if(kind&&kind->kind!=candidate->kind){++stats.skipped;return;}kind=candidate;}}}
  if(!kind)return;
  QVector<Chain> outerParts,innerParts;bool ok=true;for(const auto&m:members){check();if(m.type==1&&(m.role.isEmpty()||m.role=="outer"||m.role=="inner")){relationWays.insert(m.id);auto c=way(m.id);if(c.ids.isEmpty())ok=false;else(m.role=="inner"?innerParts:outerParts).append(std::move(c));}else if(m.role=="outer"||m.role=="inner")ok=false;}
  QVector<Chain> outers,inners;if(!ok||!assemble(std::move(outerParts),outers,check)||(!innerParts.isEmpty()&&!assemble(std::move(innerParts),inners,check))){++stats.skipped;return;}
  QVector<MapArea> areas;for(const auto&o:outers){auto a=*kind;a.outer=o.points;areas.append(std::move(a));}
  for(const auto& hole:inners){int owner=-1;double smallest=1e100;for(int i=0;i<areas.size();++i){bool contains=true;for(int j=0;j<hole.points.size()-1;++j)if(!inside(hole.points[j],areas[i].outer)){contains=false;break;}if(contains){double area=0;const auto origin=areas[i].outer.front();for(int j=1;j<areas[i].outer.size();++j){auto p=areas[i].outer[j-1]-origin,q=areas[i].outer[j]-origin;area+=p.x()*q.y()-q.x()*p.y();}if(std::abs(area)<smallest){smallest=std::abs(area);owner=i;}}}
   if(owner<0){++stats.skipped;return;}areas[owner].holes.append(hole.points);}
  for(const auto&a:areas){check();emitArea(a);++stats.emitted;}
 };
 if(osmx){for(quint32 i=0;i<ds->relationCount();++i){check();QVector<Member> members;for(const auto&m:ds->relationMembers(i))members.append({m.ref,ds->stringAt(m.role),int(m.type)});relation(interned(ds->relationTags(i)),members);}}
 else{detail::Statement s(*db,"SELECT id,tags FROM relations ORDER BY id");while(s.row()){check();auto tags=decode(s.text(1));if(tags.value("type")!="multipolygon")continue;QVector<Member> members;detail::Statement m(*db,"SELECT member_id,role,type FROM relation_members WHERE relation_id=? ORDER BY seq");m.integer(1,s.integer(0));while(m.row())members.append({m.integer(0),m.text(1),int(m.integer(2))});relation(tags,members);}}
 auto single=[&](qint64 id,const Tags& tags){check();if(relationWays.contains(id))return;auto a=classify(tags);if(!a)return;auto c=way(id);if(!valid(c)){++stats.skipped;return;}a->outer=std::move(c.points);emitArea(*a);++stats.emitted;};
 if(osmx){for(quint32 i=0;i<ds->wayCount();++i)single(ds->wayAt(i).id,interned(ds->wayTags(i)));}
 else{detail::Statement s(*db,"SELECT id,tags FROM ways ORDER BY id");while(s.row())single(s.integer(0),decode(s.text(1)));}
 return stats;
}
}
