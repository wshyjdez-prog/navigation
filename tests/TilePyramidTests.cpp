#include "ui/map/TilePyramid.h"
#include "ui/map/MapFrame.h"
#include "pal/linux/renderer/RoadPainter.h"
#include "ui/common/Theme.h"
#include "data/import/Sqlite.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QDirIterator>
using namespace p1;
class PyramidChecks:public QObject{
 Q_OBJECT
private slots:
 void areaRingsAndTiles(){
  QTemporaryDir dir;QVERIFY(dir.isValid());auto path=dir.filePath("areas.sqlite");
  {data::detail::Db db(path);db.exec(R"SQL(
   CREATE TABLE nodes(id INTEGER PRIMARY KEY,lon REAL,lat REAL);
   CREATE TABLE ways(id INTEGER PRIMARY KEY,tags TEXT);
   CREATE TABLE way_nodes(way_id INTEGER,seq INTEGER,node_id INTEGER);
   CREATE TABLE relations(id INTEGER PRIMARY KEY,tags TEXT);
   CREATE TABLE relation_members(relation_id INTEGER,seq INTEGER,type INTEGER,member_id INTEGER,role TEXT);
   INSERT INTO nodes VALUES(1,114.295,30.585),(2,114.315,30.585),(3,114.315,30.605),(4,114.295,30.605),
    (5,114.303,30.593),(6,114.307,30.593),(7,114.307,30.597),(8,114.303,30.597),
    (9,114.295,30.59),(10,114.315,30.59);
   INSERT INTO ways VALUES(10,'[]'),(11,'[]'),(12,'[]'),(13,'[["building","yes"]]'),
    (14,'[["landuse","grass"]]'),(15,'[["highway","residential"]]');
   INSERT INTO way_nodes VALUES(10,0,1),(10,1,2),(10,2,3),(11,0,1),(11,1,4),(11,2,3),
    (12,0,5),(12,1,6),(12,2,7),(12,3,8),(12,4,5),
    (13,0,5),(13,1,6),(13,2,7),(13,3,8),(13,4,5),(14,0,1),(14,1,2),
    (15,0,9),(15,1,10);
   INSERT INTO relations VALUES(1,'[["type","multipolygon"],["natural","water"]]'),
    (2,'[["type","multipolygon"],["natural","water"]]');
   INSERT INTO relation_members VALUES(1,0,1,11,'outer'),(1,1,1,10,'outer'),(1,2,1,12,'inner'),(2,0,1,999,'outer');
  )SQL");}
  app::ImportResult entry;entry.sourceFormat="osm";entry.cachePath=path;entry.sourceSha256="areas-v1";entry.datasetId="areas";
  QVector<data::MapArea> areas;auto token=std::make_shared<std::atomic_bool>(false);
  auto stats=data::visitMapAreas(entry,{},[&](const auto&a){areas.append(a);},token);
  QCOMPARE(stats.emitted,quint64(2));QCOMPARE(stats.skipped,quint64(2));QCOMPARE(areas[0].kind,data::MapAreaKind::Water);QCOMPARE(areas[0].holes.size(),1);
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
  pyramid.request(view,{512,512},false);QTRY_VERIFY_WITH_TIMEOUT(frames.size()>=2,15000);auto light=qvariant_cast<QImage>(frames.last()[0]);
  QCOMPARE(pixel(light,{114.305,30.595}),ui::mapColors(false).land);QCOMPARE(pixel(light,{114.30,30.595}),ui::mapColors(false).water);
  // A subpixel road is antialiased; its centre must brighten the water below.
  QVERIFY(pixel(light,{114.30,30.59}).red()>ui::mapColors(false).water.red()+20);
  int count=frames.size();pyramid.request(view,{1024,1024},true);QTRY_VERIFY_WITH_TIMEOUT(frames.size()>=count+2,15000);auto dark=qvariant_cast<QImage>(frames.last()[0]);
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
 void coordinates(){for(auto geo:{QPointF(114.3,30.5),QPointF(-120,-45),QPointF(0,0)}){auto p=ui::mapUnproject(ui::mapProject(geo));QVERIFY(QLineF(p,geo).length()<1e-9);}}
 void tiles(){
  QTemporaryDir dir;QVERIFY(dir.isValid());
  data::OsmxDataset::Data d;d.info.datasetId="fixture";d.nodes={{1,30.59,114.3,0,0},{2,30.6,114.31,0,0}};
  data::OsmxWay w;w.id=1;w.refCount=2;w.attributes.roadClass=data::RoadClass::Residential;d.ways.append(w);d.wayNodes={0,1};auto ds=data::OsmxDataset::create(std::move(d));QVERIFY(ds);
  app::ImportResult entry;entry.datasetId="fixture";entry.sourceSha256="fixture";entry.sourceFormat="osmx";entry.cachePath="fixture";
  auto center=ui::mapProject({114.305,30.595});QRectF view(center-QPointF(1000,1000),QSizeF(2000,2000));
  ui::TilePyramid tiles(nullptr,dir.path());QSignalSpy frames(&tiles,&ui::TilePyramid::frameReady);QSignalSpy statuses(&tiles,&ui::TilePyramid::statusChanged);
  tiles.setDataset(entry,ds);tiles.request(view,{512,512},false);
  QTRY_VERIFY_WITH_TIMEOUT(frames.count()>=2,15000);auto stats=qvariant_cast<ui::PyramidStats>(frames.last()[2]);QVERIFY(stats.tiles>0);QVERIFY(stats.zoom>10);
  auto light=qvariant_cast<QImage>(frames.last()[0]);QVERIFY(!light.isNull());
  int count=frames.count();tiles.request(view,{512,512},false);QTRY_VERIFY_WITH_TIMEOUT(frames.count()>=count+2,15000);
  stats=qvariant_cast<ui::PyramidStats>(frames.last()[2]);QCOMPARE(stats.hits,stats.tiles);
  count=frames.count();auto detail=QRectF(center-QPointF(500,500),QSizeF(1000,1000));tiles.request(detail,{512,512},false);
  QTRY_VERIFY_WITH_TIMEOUT(frames.count()>=count+2,15000);
  QVERIFY(qvariant_cast<ui::PyramidStats>(frames[count][2]).parents>0);
  count=frames.count();tiles.request(view,{512,512},true);QTRY_VERIFY_WITH_TIMEOUT(frames.count()>=count+2,15000);QVERIFY(qvariant_cast<QImage>(frames.last()[0])!=light);
  count=frames.count();auto latest=view.translated(4000,0);tiles.request(view,{512,512},false);tiles.request(latest,{512,512},false);
  QTRY_VERIFY_WITH_TIMEOUT(frames.count()>=count+2,15000);QCOMPARE(frames.last()[1].toRectF(),latest);
  QDirIterator it(dir.path(),{"*.png"},QDir::Files,QDirIterator::Subdirectories);QVERIFY(it.hasNext());
  // A new provider must read persistent tiles without re-rasterizing them.
  ui::TilePyramid reopened(nullptr,dir.path());QSignalSpy restored(&reopened,&ui::TilePyramid::frameReady);
  reopened.setDataset(entry,ds);reopened.request(latest,{512,512},false);
  QTRY_VERIFY_WITH_TIMEOUT(restored.count()>=2,15000);
  auto persisted=qvariant_cast<ui::PyramidStats>(restored.last()[2]);QCOMPARE(persisted.hits,persisted.tiles);
 }
};
QTEST_MAIN(PyramidChecks)
#include "TilePyramidTests.moc"
