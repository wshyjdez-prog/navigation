#include "ui/map/TilePyramid.h"
#include "ui/map/AreaLabels.h"
#include "ui/map/MapFrame.h"
#include "pal/linux/renderer/RoadPainter.h"
#include "ui/common/Theme.h"
#include "data/import/OsmImportParser.h"
#include "contracts/ImportedDataset.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QDirIterator>
#include <QRandomGenerator>
#include <QSet>
using namespace p1;
namespace {
// Progressive frames precede the request's final (complete) frame.
bool done(const QSignalSpy& frames,int before){return frames.size()>before&&qvariant_cast<ui::PyramidStats>(frames.last()[2]).complete;}
// Frames may cover more than the request (overscan); cut out the requested view.
QImage visible(const QSignalSpy& frames,QRectF view,QSize size){return ui::reprojectMapFrame(qvariant_cast<QImage>(frames.last()[0]),frames.last()[1].toRectF(),view,size,Qt::magenta);}
}
class PyramidChecks:public QObject{
 Q_OBJECT
private slots:
 void areaRingsAndTiles(){
  // Imported through the real libosmium parser: a lake split over two outer
  // ways with an island, a building on the island, a road, an unclosed
  // landuse way, a multipolygon with a missing member and a POI on the island.
  QTemporaryDir dir;QVERIFY(dir.isValid());auto osmFile=dir.filePath("areas.osm");
  {QFile f(osmFile);QVERIFY(f.open(QIODevice::WriteOnly));f.write(R"(<osm version="0.6">
   <node id="1" lon="114.295" lat="30.585"/><node id="2" lon="114.315" lat="30.585"/>
   <node id="3" lon="114.315" lat="30.605"/><node id="4" lon="114.295" lat="30.605"/>
   <node id="5" lon="114.303" lat="30.593"/><node id="6" lon="114.307" lat="30.593"/>
   <node id="7" lon="114.307" lat="30.597"/><node id="8" lon="114.303" lat="30.597"/>
   <node id="9" lon="114.295" lat="30.59"/><node id="10" lon="114.315" lat="30.59"/>
   <node id="20" lon="114.305" lat="30.595"><tag k="amenity" v="restaurant"/><tag k="name" v="岛上餐厅"/></node>
   <way id="10"><nd ref="1"/><nd ref="2"/><nd ref="3"/></way>
   <way id="11"><nd ref="1"/><nd ref="4"/><nd ref="3"/></way>
   <way id="12"><nd ref="5"/><nd ref="6"/><nd ref="7"/><nd ref="8"/><nd ref="5"/></way>
   <way id="13"><nd ref="5"/><nd ref="6"/><nd ref="7"/><nd ref="8"/><nd ref="5"/><tag k="building" v="yes"/></way>
   <way id="14"><nd ref="1"/><nd ref="2"/><tag k="landuse" v="grass"/></way>
   <way id="15"><nd ref="9"/><nd ref="10"/><tag k="highway" v="residential"/></way>
   <relation id="1"><member type="way" ref="11" role="outer"/><member type="way" ref="10" role="outer"/>
    <member type="way" ref="12" role="inner"/><tag k="type" v="multipolygon"/><tag k="natural" v="water"/></relation>
   <relation id="2"><member type="way" ref="999" role="outer"/><tag k="type" v="multipolygon"/><tag k="natural" v="water"/></relation>
  </osm>)");}
  const auto entry=data::OsmImportParser(dir.filePath("cache")).parse(osmFile,{},{},{});QVERIFY2(entry.ok,qPrintable(entry.error));
  QCOMPARE(entry.stats.pois,quint64(1));QCOMPARE(data::ImportedDataset(entry.cachePath).findPois("餐厅").size(),1);
  QVector<data::MapArea> areas;auto token=std::make_shared<std::atomic_bool>(false);
  auto stats=data::visitMapAreas(entry,{},[&](const auto&a){areas.append(a);},token);
  QCOMPARE(stats.emitted,quint64(2));QCOMPARE(stats.skipped,quint64(1));QCOMPARE(areas[0].kind,data::MapAreaKind::Water);QCOMPARE(areas[0].holes.size(),1);
  QCOMPARE(areas[1].kind,data::MapAreaKind::Building);
  data::OsmxDataset::Data source;source.info.datasetId="area-osmx";source.strings={"building","yes"};
  source.nodes={{1,30.59,114.3,0,0},{2,30.59,114.31,0,0},{3,30.6,114.31,0,0},{4,30.6,114.3,0,0}};
  data::OsmxWay building;building.id=20;building.refCount=5;building.tagCount=1;source.ways.append(building);source.wayNodes={0,1,2,3,0};source.wayTags={{0,1}};
  auto osmx=data::OsmxDataset::create(std::move(source));QVERIFY(osmx);app::ImportResult osmxEntry;osmxEntry.sourceFormat="osmx";
  QVector<data::MapArea> osmxAreas;auto osmxStats=data::visitMapAreas(osmxEntry,osmx,[&](const auto&a){osmxAreas.append(a);},{});
  QCOMPARE(osmxStats.emitted,quint64(1));QCOMPARE(osmxAreas[0].kind,data::MapAreaKind::Building);
  int roadCount=0;data::visitMapRoads(osmxEntry,osmx,[&](const auto&){++roadCount;},{});QCOMPARE(roadCount,0);
  token->store(true);QVERIFY_EXCEPTION_THROWN(data::visitMapAreas(entry,{},[](const auto&){},token),std::runtime_error);
  ui::TilePyramid pyramid(nullptr,dir.filePath("tiles"));pyramid.setDataset(entry,{});QSignalSpy frames(&pyramid,&ui::TilePyramid::frameReady);
  auto center=ui::mapProject({114.305,30.595});QRectF view(center-QPointF(2000,2000),QSizeF(4000,4000));
  auto pixel=[&](const QImage& image,QPointF geo){auto p=ui::mapProject(geo);return image.pixelColor(qRound((p.x()-view.left())/view.width()*image.width()),qRound((view.bottom()-p.y())/view.height()*image.height()));};
  pyramid.request(view,{512,512},false);QTRY_VERIFY_WITH_TIMEOUT(done(frames,0),15000);auto light=visible(frames,view,{512,512});
  // The POI at the island centre is stored but never drawn: plain land here,
  // and plain building colour once the building level is visible.
  QCOMPARE(pixel(light,{114.305,30.595}),ui::mapColors(false).land);QCOMPARE(pixel(light,{114.30,30.595}),ui::mapColors(false).water);
  // A subpixel road is antialiased; its centre must brighten the water below.
  QVERIFY(pixel(light,{114.30,30.59}).red()>ui::mapColors(false).water.red()+20);
  int count=frames.size();pyramid.request(view,{1024,1024},true);QTRY_VERIFY_WITH_TIMEOUT(done(frames,count),15000);auto dark=visible(frames,view,{1024,1024});
  QCOMPARE(pixel(dark,{114.305,30.595}),ui::mapColors(true).building);QCOMPARE(pixel(dark,{114.30,30.595}),ui::mapColors(true).water);
 }
 void retainedBaseMovesEveryRegionTogether(){
  const QColor land("#EEEEEE"),city("#2355CC"),other("#228844");
  QImage old(80,40,QImage::Format_ARGB32_Premultiplied);old.fill(land);
  {QPainter p(&old);p.fillRect(QRect(8,16,8,8),city);p.fillRect(QRect(56,16,8,8),other);}
  const auto moved=ui::reprojectMapFrame(old,{0,0,80,40},{20,0,80,40},{80,40},land);
  QCOMPARE(moved.pixelColor(40,20),other);
  QCOMPARE(moved.pixelColor(10,20),land);
  QCOMPARE(moved.pixelColor(60,20),land);
  const auto zoomed=ui::reprojectMapFrame(old,{0,0,80,40},{20,10,40,20},{80,40},land);
  QCOMPARE(zoomed.pixelColor(76,20),other);
  QCOMPARE(zoomed.pixelColor(10,20),land);
  // An uncovered view stays empty; no initial-region roads are synthesized.
  const auto empty=ui::reprojectMapFrame(old,{0,0,80,40},{200,0,80,40},{80,40},land);
  QCOMPARE(empty.pixelColor(40,20),land);
 }
 void junctionCasingsStayOutsideRoadInteriors(){
  for(bool dark:{false,true}) {
   const auto colors=ui::mapColors(dark);QVector<renderer::RoadStroke> roads;
   QPainterPath horizontal;horizontal.moveTo(20,90);horizontal.lineTo(180,90);
   QPainterPath north;north.moveTo(100,90);north.lineTo(100,20);
   QPainterPath south;south.moveTo(100,90);south.lineTo(100,160);
   roads={{horizontal,14,colors.road},{north,6,colors.road},{south,6,colors.road}};
   auto render=[&](bool legacy){QImage image(200,180,QImage::Format_ARGB32_Premultiplied);image.fill(colors.land);QPainter p(&image);p.setRenderHint(QPainter::Antialiasing);
    if(legacy)for(const auto& road:roads){p.setPen(QPen(colors.roadEdge,road.width+2,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.drawPath(road.path);p.setPen(QPen(road.fill,road.width,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.drawPath(road.path);}
    else renderer::paintRoads(p,roads,colors.roadEdge);
    p.end();return image;};
   const auto before=render(true),after=render(false);
   QCOMPARE(after.pixelColor(103,90),colors.road);
   QVERIFY(before.pixelColor(103,90)!=after.pixelColor(103,90));
   // Preserve the outside road boundary and background.
   QCOMPARE(after.pixelColor(60,82),before.pixelColor(60,82));
   QCOMPARE(after.pixelColor(10,10),colors.land);
  }
 }
 void areaLabelAnchorsStayInside(){
  // U shape: the bounding-box centre and the centroid fall into the notch.
  const QVector<QPointF> u{{0,0},{300,0},{300,300},{200,300},{200,100},{100,100},{100,300},{0,300},{0,0}};
  auto [p,d]=ui::poleOfInaccessibility(u,{});
  const bool inArm=(p.x()<100||p.x()>200||p.y()<100)&&p.x()>0&&p.x()<300&&p.y()>0&&p.y()<300;
  // Corner of the base bar and an arm: clearance sqrt(2)*41.4 = 58.6.
  QVERIFY2(inArm&&d>55&&d<=58.6,qPrintable(QString("%1,%2 d=%3").arg(p.x()).arg(p.y()).arg(d)));
  // A centred hole pushes the anchor into the remaining ring.
  const QVector<QPointF> square{{0,0},{400,0},{400,400},{0,400},{0,0}},hole{{100,100},{300,100},{300,300},{100,300},{100,100}};
  auto [q,e]=ui::poleOfInaccessibility(square,{hole});
  QVERIFY(!(q.x()>100&&q.x()<300&&q.y()>100&&q.y()<300));QVERIFY(e>55&&e<=58.6); // ring corner, same geometry as above
  QVERIFY(!ui::makeAreaLabel(data::MapAreaKind::Water,8,"  ",square,{}));
  auto named=ui::makeAreaLabel(data::MapAreaKind::Water,8," East  Lake ",square,{});QVERIFY(named);QCOMPARE(named->name,QString("East Lake"));
 }
 void areaLabelLevels(){
  using K=data::MapAreaKind;
  // Clearance needed for 10 px at z: 10*world/(256*2^z) metres.
  const double world=40075016.68557849;auto needed=[&](int z){return 10*world/(256*std::ldexp(1.,z));};
  QCOMPARE(ui::areaLabelMinZoom(K::Water,8,needed(12)*1.01),12);
  QCOMPARE(ui::areaLabelMinZoom(K::Water,8,needed(12)*0.99),13);
  QCOMPARE(ui::areaLabelMinZoom(K::Water,8,needed(3)),8);   // kind floor
  QCOMPARE(ui::areaLabelMinZoom(K::Building,15,needed(10)),16); // building names two levels later
  QCOMPARE(ui::areaLabelMinZoom(K::Green,10,needed(18)*0.5),-1); // never large enough
  QCOMPARE(ui::areaLabelMinZoom(K::Landuse,11,0),-1);
  int last=0;for(double c:{20.,80.,320.,1280.,5120.}){int z=ui::areaLabelMinZoom(K::Water,0,c);QVERIFY(z>=0);if(last)QVERIFY(z<=last);last=z;}
 }
 void strictPlacementIsWindowIndependent(){
  QRandomGenerator random(20261111);QVector<ui::AreaLabelCandidate> all;
  for(int i=0;i<900;++i){ui::AreaLabel l;l.id=i+1;l.kind=data::MapAreaKind(random.bounded(4));l.road=random.bounded(3)==0;l.clearance=random.bounded(1000);
   l.name=QString("Area %1").arg(QString(random.bounded(1,30),QChar('x')));all.append({l,QPointF(random.bounded(2048),random.bounded(2048))});}
  auto ids=[](const QVector<ui::PlacedAreaLabel>& placed,QRectF window){QSet<qint64> out;for(const auto& p:placed)if(p.box.intersects(window))out.insert(p.id);return out;};
  const auto full=ui::placeAreaLabels(all);QVERIFY(!full.isEmpty());
  const qreal margin=ui::areaLabelQueryMarginPixels();
  for(QRectF window:{QRectF(768,768,256,256),QRectF(0,0,256,256),QRectF(1536,1024,256,256)}){
   QVector<ui::AreaLabelCandidate> local;for(const auto& c:all)if(window.adjusted(-margin,-margin,margin,margin).contains(c.pixel))local.append(c);
   QCOMPARE(ids(ui::placeAreaLabels(local),window),ids(full,window));
  }
  QHash<qint64,QString> names;for(const auto& c:all)names.insert(c.label.id,c.label.name);
  for(int i=0;i<full.size();++i)for(int j=i+1;j<full.size();++j){QVERIFY(!full[i].box.intersects(full[j].box));
   if(names[full[i].id]==names[full[j].id])QVERIFY(QLineF(full[i].box.center(),full[j].box.center()).length()>=ui::kSameNameRadiusPixels);}
 }
 void sameNameRoadsMerge(){
  // A street split into ways: the longest way wins, nearby repeats are dropped,
  // a repeat beyond the merge radius stays, other names are unaffected.
  auto road=[](qint64 id,QString name,double length,QPointF pixel){ui::AreaLabel l;l.id=id;l.road=true;l.name=name;l.clearance=length;return ui::AreaLabelCandidate{l,pixel};};
  const auto placed=ui::placeAreaLabels({road(1,"Main St",200,{100,100}),road(2,"Main St",200,{200,100}),road(3,"Main St",400,{330,100}),
   road(4,"Main St",300,{330+ui::kSameNameRadiusPixels+20,100}),road(5,"Side Rd",50,{230,160})});
  QSet<qint64> ids;for(const auto& p:placed)ids.insert(p.id);
  QCOMPARE(ids,(QSet<qint64>{3,4,5}));
  // makeRoadLabel anchors at half the length, not at the middle vertex.
  auto label=ui::makeRoadLabel(12,"  Main  St ",{{0,0},{10,0},{20,0},{100,0}});QVERIFY(label);
  QCOMPARE(label->anchor,QPointF(50,0));QCOMPARE(label->minZoom,ui::kRoadLabelMinZoom);QCOMPARE(label->name,QString("Main St"));QVERIFY(label->road);
  QVERIFY(!ui::makeRoadLabel(12,"",{{0,0},{1,0}}));QCOMPARE(ui::makeRoadLabel(17,"x",{{0,0},{1,0}})->minZoom,17);
 }
 void areaNamesFollowLevels(){
  // A named square lake whose centre sits exactly on a z13 tile column border.
  const double world=40075016.68557849,step=world/8192;const double lat=30.595;
  const double border=-world/2+std::floor((ui::mapProject({114.3,lat}).x()+world/2)/step)*step;
  const double lon=std::round(ui::mapUnproject({border,ui::mapProject({114.3,lat}).y()}).x()*1e7)/1e7;
  QTemporaryDir dir;QVERIFY(dir.isValid());auto osmFile=dir.filePath("lake.osm");
  {QFile f(osmFile);QVERIFY(f.open(QIODevice::WriteOnly));auto n=[](int id,double x,double y){return QString("<node id=\"%1\" lon=\"%2\" lat=\"%3\"/>").arg(id).arg(x,0,'f',7).arg(y,0,'f',7);};
   f.write(QString("<osm version=\"0.6\">%1%2%3%4<way id=\"1\"><nd ref=\"1\"/><nd ref=\"2\"/><nd ref=\"3\"/><nd ref=\"4\"/><nd ref=\"1\"/><tag k=\"natural\" v=\"water\"/><tag k=\"name\" v=\"East Lake\"/></way></osm>")
    .arg(n(1,lon-.01,lat-.009),n(2,lon+.01,lat-.009),n(3,lon+.01,lat+.009),n(4,lon-.01,lat+.009)).toUtf8());}
  const auto entry=data::OsmImportParser(dir.filePath("cache")).parse(osmFile,{},{},{});QVERIFY2(entry.ok,qPrintable(entry.error));
  ui::TilePyramid pyramid(nullptr,dir.filePath("tiles"));pyramid.setDataset(entry,{});QSignalSpy frames(&pyramid,&ui::TilePyramid::frameReady);
  const auto centre=ui::mapProject({lon,lat});
  auto render=[&](double width){int count=frames.size();QRectF view(centre-QPointF(width/2,width/2),QSizeF(width,width));pyramid.request(view,{512,512},false);
   [&]{QTRY_VERIFY_WITH_TIMEOUT(done(frames,count),20000);}();return std::make_pair(visible(frames,view,{512,512}),qvariant_cast<ui::PyramidStats>(frames.last()[2]));};
  const auto water=ui::mapColors(false).water;const int waterSum=water.red()+water.green()+water.blue();
  auto ink=[&](const QImage& image,QRect r){int n=0;for(int y=r.top();y<=r.bottom();++y)for(int x=r.left();x<=r.right();++x){auto c=image.pixelColor(x,y);if(c.red()+c.green()+c.blue()<waterSum-150)++n;}return n;};
  auto [low,lowStats]=render(50000);QCOMPARE(lowStats.zoom,10);QCOMPARE(ink(low,QRect(236,246,40,20)),0);
  auto [mid,midStats]=render(12000);QCOMPARE(midStats.zoom,12);QVERIFY(ink(mid,QRect(226,248,60,16))>15);
  // Border crossing: both halves of the name are present around x=256.
  auto [seam,seamStats]=render(6000);QCOMPARE(seamStats.zoom,13);
  QVERIFY2(ink(seam,QRect(236,248,18,16))>5&&ink(seam,QRect(258,248,18,16))>5,qPrintable(QString("%1/%2").arg(ink(seam,QRect(236,248,18,16))).arg(ink(seam,QRect(258,248,18,16)))));
 }
 void splitStreetShowsOneName(){
  // Three consecutive ways of one street (200 m, 200 m, 400 m): at z16 their
  // midpoints are < 256 px apart, so only the longest way's name is drawn.
  const double lat=30.595,lon0=114.30;const double m=1.0/(111320*std::cos(lat*std::acos(-1.)/180));
  QTemporaryDir dir;QVERIFY(dir.isValid());auto osmFile=dir.filePath("street.osm");
  {QFile f(osmFile);QVERIFY(f.open(QIODevice::WriteOnly));QString xml="<osm version=\"0.6\">";const double xs[]={0,200,400,800};
   for(int i=0;i<4;++i)xml+=QString("<node id=\"%1\" lon=\"%2\" lat=\"%3\"/>").arg(i+1).arg(lon0+xs[i]*m,0,'f',7).arg(lat,0,'f',7);
   for(int i=0;i<3;++i)xml+=QString("<way id=\"%1\"><nd ref=\"%1\"/><nd ref=\"%2\"/><tag k=\"highway\" v=\"residential\"/><tag k=\"name\" v=\"Main Street\"/></way>").arg(i+1).arg(i+2);
   f.write((xml+"</osm>").toUtf8());}
  const auto entry=data::OsmImportParser(dir.filePath("cache")).parse(osmFile,{},{},{});QVERIFY2(entry.ok,qPrintable(entry.error));
  ui::TilePyramid pyramid(nullptr,dir.filePath("tiles"));pyramid.setDataset(entry,{});QSignalSpy frames(&pyramid,&ui::TilePyramid::frameReady);
  const auto a=ui::mapProject({lon0,lat}),b=ui::mapProject({lon0+800*m,lat});const QPointF centre=(a+b)/2;const double width=1000;
  QRectF view(centre-QPointF(width/2,width/2),QSizeF(width,width));pyramid.request(view,{512,512},false);QTRY_VERIFY_WITH_TIMEOUT(done(frames,0),20000);
  const auto image=visible(frames,view,{512,512});QCOMPARE(qvariant_cast<ui::PyramidStats>(frames.last()[2]).zoom,16);
  auto at=[&](double metres){auto p=ui::mapProject({lon0+metres*m,lat});return QPoint(qRound((p.x()-view.left())/view.width()*512),qRound((view.bottom()-p.y())/view.height()*512));};
  auto ink=[&](QPoint c){int n=0;for(int y=c.y()-8;y<=c.y()+8;++y)for(int x=c.x()-20;x<=c.x()+20;++x){auto q=image.pixelColor(x,y);if(q.red()+q.green()+q.blue()<450)++n;}return n;};
  QVERIFY2(ink(at(600))>15,qPrintable(QString::number(ink(at(600)))));QCOMPARE(ink(at(100)),0);QCOMPARE(ink(at(300)),0);
 }
 void panEdgesAreCovered(){
  // A lake covering the whole neighbourhood: any uncovered pixel shows land.
  const double lat=30.595,lon=114.30;
  QTemporaryDir dir;QVERIFY(dir.isValid());auto osmFile=dir.filePath("lake.osm");
  {QFile f(osmFile);QVERIFY(f.open(QIODevice::WriteOnly));auto n=[](int id,double x,double y){return QString("<node id=\"%1\" lon=\"%2\" lat=\"%3\"/>").arg(id).arg(x,0,'f',7).arg(y,0,'f',7);};
   f.write(QString("<osm version=\"0.6\">%1%2%3%4<way id=\"1\"><nd ref=\"1\"/><nd ref=\"2\"/><nd ref=\"3\"/><nd ref=\"4\"/><nd ref=\"1\"/><tag k=\"natural\" v=\"water\"/></way></osm>")
    .arg(n(1,lon-.08,lat-.06),n(2,lon+.08,lat-.06),n(3,lon+.08,lat+.06),n(4,lon-.08,lat+.06)).toUtf8());}
  const auto entry=data::OsmImportParser(dir.filePath("cache")).parse(osmFile,{},{},{});QVERIFY2(entry.ok,qPrintable(entry.error));
  ui::TilePyramid pyramid(nullptr,dir.filePath("tiles"));pyramid.setDataset(entry,{});QSignalSpy frames(&pyramid,&ui::TilePyramid::frameReady);
  const QSize size(512,512);const auto centre=ui::mapProject({lon,lat});QRectF view(centre-QPointF(1500,1500),QSizeF(3000,3000));
  pyramid.request(view,size,false);QTRY_VERIFY_WITH_TIMEOUT(done(frames,0),20000);QCOMPARE(qvariant_cast<ui::PyramidStats>(frames.last()[2]).zoom,14);
  const auto water=ui::mapColors(false).water;
  for(QRectF next:{view.translated(.4*view.width(),0),QRectF(view.center()-QPointF(1875,1875),QSizeF(3750,3750))}){
   const int before=frames.size();pyramid.request(next,size,false);QTRY_VERIFY_WITH_TIMEOUT(frames.size()>before,20000);
   const auto first=qvariant_cast<ui::PyramidStats>(frames[before][2]);QCOMPARE(first.blank,0);
   const auto image=ui::reprojectMapFrame(qvariant_cast<QImage>(frames[before][0]),frames[before][1].toRectF(),next,size,Qt::magenta);
   for(QPoint p:{QPoint(3,3),QPoint(508,3),QPoint(3,508),QPoint(508,508),QPoint(256,256)})QCOMPARE(image.pixelColor(p),water);
   QTRY_VERIFY_WITH_TIMEOUT(done(frames,before),20000);view=next;
  }
 }
 void clarityRendersDenserTiles(){
  // Same view at low and high clarity: tiles are 256 vs 512 px, the level is
  // unchanged and a name keeps its on-screen width (styles scale with density).
  const double lat=30.595,lon=114.30;
  QTemporaryDir dir;QVERIFY(dir.isValid());auto osmFile=dir.filePath("lake.osm");
  {QFile f(osmFile);QVERIFY(f.open(QIODevice::WriteOnly));auto n=[](int id,double x,double y){return QString("<node id=\"%1\" lon=\"%2\" lat=\"%3\"/>").arg(id).arg(x,0,'f',7).arg(y,0,'f',7);};
   f.write(QString("<osm version=\"0.6\">%1%2%3%4<way id=\"1\"><nd ref=\"1\"/><nd ref=\"2\"/><nd ref=\"3\"/><nd ref=\"4\"/><nd ref=\"1\"/><tag k=\"natural\" v=\"water\"/><tag k=\"name\" v=\"East Lake\"/></way></osm>")
    .arg(n(1,lon-.01,lat-.009),n(2,lon+.01,lat-.009),n(3,lon+.01,lat+.009),n(4,lon-.01,lat+.009)).toUtf8());}
  const auto entry=data::OsmImportParser(dir.filePath("cache")).parse(osmFile,{},{},{});QVERIFY2(entry.ok,qPrintable(entry.error));
  ui::TilePyramid pyramid(nullptr,dir.filePath("tiles"));pyramid.setDataset(entry,{});QSignalSpy frames(&pyramid,&ui::TilePyramid::frameReady);
  QCOMPARE(pyramid.clarity(),ui::MapClarity::Low);
  const auto centre=ui::mapProject({lon,lat});const QRectF view(centre-QPointF(6000,6000),QSizeF(12000,12000));const QSize size(512,512);
  const int waterSum=[]{auto w=ui::mapColors(false).water;return w.red()+w.green()+w.blue();}();
  auto inkWidth=[&](const QImage& image){int left=9999,right=-1;for(int y=236;y<=276;++y)for(int x=176;x<=336;++x){auto c=image.pixelColor(x,y);if(c.red()+c.green()+c.blue()<waterSum-150){left=std::min(left,x);right=std::max(right,x);}}return right-left;};
  pyramid.request(view,size,false);QTRY_VERIFY_WITH_TIMEOUT(done(frames,0),20000);
  const int lowZoom=qvariant_cast<ui::PyramidStats>(frames.last()[2]).zoom;const int lowWidth=inkWidth(visible(frames,view,size));QVERIFY(lowWidth>20);
  int count=frames.size();pyramid.setClarity(ui::MapClarity::High);QCOMPARE(pyramid.clarity(),ui::MapClarity::High);
  QTRY_VERIFY_WITH_TIMEOUT(done(frames,count),20000);
  QCOMPARE(qvariant_cast<ui::PyramidStats>(frames.last()[2]).zoom,lowZoom);
  const int highWidth=inkWidth(visible(frames,view,size));QVERIFY2(std::abs(highWidth-lowWidth)<=lowWidth/4,qPrintable(QString("%1/%2").arg(lowWidth).arg(highWidth)));
  int high=0,low=0;QDirIterator it(dir.filePath("tiles"),{"*.png"},QDir::Files,QDirIterator::Subdirectories);
  while(it.hasNext()){const auto path=it.next();const QImage tile(path);if(path.contains("-d512/")){QCOMPARE(tile.size(),QSize(512,512));++high;}else if(path.contains("-d256/")){QCOMPARE(tile.size(),QSize(256,256));++low;}}
  QVERIFY(high>0&&low>0);
  QCOMPARE(ui::clarityDensity(ui::MapClarity::Medium),1.5);
 }
 void coordinates(){for(auto geo:{QPointF(114.3,30.5),QPointF(-120,-45),QPointF(0,0)}){auto p=ui::mapUnproject(ui::mapProject(geo));QVERIFY(QLineF(p,geo).length()<1e-9);}}
 void tiles(){
  QTemporaryDir dir;QVERIFY(dir.isValid());
  data::OsmxDataset::Data d;d.info.datasetId="fixture";d.nodes={{1,30.59,114.3,0,0},{2,30.6,114.31,0,0}};
  data::OsmxWay w;w.id=1;w.refCount=2;w.attributes.roadClass=data::RoadClass::Residential;d.ways.append(w);d.wayNodes={0,1};auto ds=data::OsmxDataset::create(std::move(d));QVERIFY(ds);
  app::ImportResult entry;entry.datasetId="fixture";entry.sourceSha256="fixture";entry.sourceFormat="osmx";entry.cachePath="fixture";
  auto center=ui::mapProject({114.305,30.595});QRectF view(center-QPointF(1000,1000),QSizeF(2000,2000));
  ui::TilePyramid tiles(nullptr,dir.path());QSignalSpy frames(&tiles,&ui::TilePyramid::frameReady);QSignalSpy statuses(&tiles,&ui::TilePyramid::statusChanged);
  tiles.setDataset(entry,ds);tiles.request(view,{512,512},false);
  QTRY_VERIFY_WITH_TIMEOUT(done(frames,0),15000);auto stats=qvariant_cast<ui::PyramidStats>(frames.last()[2]);QVERIFY(stats.tiles>0);QVERIFY(stats.zoom>10);
  auto light=visible(frames,view,{512,512});QVERIFY(!light.isNull());
  int count=frames.count();tiles.request(view,{512,512},false);QTRY_VERIFY_WITH_TIMEOUT(done(frames,count),15000);
  stats=qvariant_cast<ui::PyramidStats>(frames.last()[2]);QCOMPARE(stats.hits,stats.tiles);
  count=frames.count();auto detail=QRectF(center-QPointF(500,500),QSizeF(1000,1000));tiles.request(detail,{512,512},false);
  QTRY_VERIFY_WITH_TIMEOUT(done(frames,count),15000);
  QVERIFY(qvariant_cast<ui::PyramidStats>(frames[count][2]).parents>0);
  count=frames.count();tiles.request(view,{512,512},true);QTRY_VERIFY_WITH_TIMEOUT(done(frames,count),15000);QVERIFY(visible(frames,view,{512,512})!=light);
  count=frames.count();auto latest=view.translated(4000,0);tiles.request(view,{512,512},false);tiles.request(latest,{512,512},false);
  QTRY_VERIFY_WITH_TIMEOUT(done(frames,count),15000);QVERIFY(frames.last()[1].toRectF().contains(latest));QVERIFY(QLineF(frames.last()[1].toRectF().center(),latest.center()).length()<1e-6);
  QDirIterator it(dir.path(),{"*.png"},QDir::Files,QDirIterator::Subdirectories);QVERIFY(it.hasNext());
  // A new provider must read persistent tiles without re-rasterizing them.
  ui::TilePyramid reopened(nullptr,dir.path());QSignalSpy restored(&reopened,&ui::TilePyramid::frameReady);
  reopened.setDataset(entry,ds);reopened.request(latest,{512,512},false);
  QTRY_VERIFY_WITH_TIMEOUT(done(restored,0),15000);
  auto persisted=qvariant_cast<ui::PyramidStats>(restored.last()[2]);QCOMPARE(persisted.hits,persisted.tiles);
 }
};
QTEST_MAIN(PyramidChecks)
#include "TilePyramidTests.moc"
