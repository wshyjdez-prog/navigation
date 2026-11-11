#include "data/import/OsmImportParser.h"
#include "contracts/ImportedDataset.h"
#include "contracts/MapGeometry.h"
#include "data/import/Sqlite.h"
#include <osmium/io/any_input.hpp>
#include <osmium/io/any_output.hpp>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

namespace {
// One POI node, one road, one lake with an island (multipolygon split over
// two outer ways plus an inner ring), one building that is also a POI, and a
// route relation that must not become an area.
QByteArray xml() {
    return "<?xml version=\"1.0\"?><osm version=\"0.6\" generator=\"P1 test\">"
      "<node id=\"1\" lat=\"30.5\" lon=\"114.3\"><tag k=\"amenity\" v=\"cafe\"/><tag k=\"name\" v=\"Test Cafe\"/><tag k=\"name:zh\" v=\"测试咖啡馆\"/></node>"
      "<node id=\"2\" lat=\"30.6\" lon=\"114.4\"><tag k=\"note\" v=\"not a poi\"/></node>"
      "<node id=\"11\" lat=\"30.50\" lon=\"114.30\"/><node id=\"12\" lat=\"30.50\" lon=\"114.34\"/>"
      "<node id=\"13\" lat=\"30.54\" lon=\"114.34\"/><node id=\"14\" lat=\"30.54\" lon=\"114.30\"/>"
      "<node id=\"15\" lat=\"30.51\" lon=\"114.31\"/><node id=\"16\" lat=\"30.51\" lon=\"114.33\"/>"
      "<node id=\"17\" lat=\"30.53\" lon=\"114.33\"/><node id=\"18\" lat=\"30.53\" lon=\"114.31\"/>"
      "<node id=\"21\" lat=\"30.60\" lon=\"114.40\"/><node id=\"22\" lat=\"30.60\" lon=\"114.42\"/>"
      "<node id=\"23\" lat=\"30.62\" lon=\"114.42\"/><node id=\"24\" lat=\"30.62\" lon=\"114.40\"/>"
      "<way id=\"10\"><nd ref=\"1\"/><nd ref=\"2\"/><tag k=\"highway\" v=\"residential\"/><tag k=\"name\" v=\"测试路\"/></way>"
      "<way id=\"30\"><nd ref=\"11\"/><nd ref=\"12\"/><nd ref=\"13\"/></way>"
      "<way id=\"31\"><nd ref=\"13\"/><nd ref=\"14\"/><nd ref=\"11\"/></way>"
      "<way id=\"32\"><nd ref=\"15\"/><nd ref=\"16\"/><nd ref=\"17\"/><nd ref=\"18\"/><nd ref=\"15\"/></way>"
      "<way id=\"40\"><nd ref=\"21\"/><nd ref=\"22\"/><nd ref=\"23\"/><nd ref=\"24\"/><nd ref=\"21\"/>"
      "<tag k=\"building\" v=\"yes\"/><tag k=\"amenity\" v=\"school\"/><tag k=\"name\" v=\"测试学校\"/></way>"
      "<relation id=\"20\"><member type=\"way\" ref=\"10\" role=\"outer\"/><tag k=\"type\" v=\"route\"/></relation>"
      "<relation id=\"50\"><member type=\"way\" ref=\"30\" role=\"outer\"/><member type=\"way\" ref=\"31\" role=\"outer\"/>"
      "<member type=\"way\" ref=\"32\" role=\"inner\"/><tag k=\"type\" v=\"multipolygon\"/><tag k=\"natural\" v=\"water\"/><tag k=\"name\" v=\"测试湖\"/></relation></osm>";
}
void write(const QString& path,const QByteArray& bytes) {
    QFile f(path);
    if(!f.open(QIODevice::WriteOnly) || f.write(bytes)!=bytes.size()) throw std::runtime_error("fixture write failed");
}
}
class ImportTests : public QObject {
    Q_OBJECT
private slots:
    void formats_data() {
        QTest::addColumn<QString>("extension");
        for(const auto& e:{".osm",".pbf",".osm.pbf",".osm.gz",".osm.bz2"}) QTest::newRow(e)<<QString(e);
    }
    void formats() {
        QFETCH(QString,extension);
        QTemporaryDir dir;
        const auto source=dir.filePath("source.osm"), target=dir.filePath("input"+extension);
        write(source,xml());
        {
            osmium::io::Reader reader(source.toStdString());
            osmium::io::Writer writer(target.toStdString(),osmium::io::overwrite::allow);
            while(auto buffer=reader.read()) writer(std::move(buffer));
            writer.close(); reader.close();
        }
        p1::data::OsmImportParser parser(dir.filePath("cache"));
        QVector<int> progress;
        const auto r=parser.parse(target,[&](int p,const QString&){progress.append(p);},{},{});
        QVERIFY2(r.ok,qPrintable(r.error)); QVERIFY(r.warnings.isEmpty());
        QCOMPARE(r.stats.nodes,quint64(14)); QCOMPARE(r.stats.ways,quint64(5));
        QCOMPARE(r.stats.relations,quint64(2)); QCOMPARE(r.stats.tags,quint64(13));
        QCOMPARE(r.stats.nodeReferences,quint64(18)); QCOMPARE(r.stats.relationMembers,quint64(4));
        QCOMPARE(r.stats.pois,quint64(2)); QCOMPARE(r.stats.roads,quint64(1));
        QCOMPARE(r.stats.areas,quint64(2)); QCOMPARE(r.stats.areasSkipped,quint64(0));
        QCOMPARE(r.formatVersion,quint32(2));
        QCOMPARE(progress.last(),100);
        QVERIFY(std::is_sorted(progress.cbegin(),progress.cend()));
        p1::data::ImportedDataset data(r.cachePath);
        const p1::data::OsmxBounds everywhere{-90,-180,90,180,true};

        // POIs: the tagged node, and the school building at its bbox centre.
        const auto pois=data.pois(everywhere); QCOMPARE(pois.size(),2);
        QCOMPARE(pois[0].osmType,0); QCOMPARE(pois[0].osmId,qint64(1));
        QCOMPARE(pois[0].category,QString("amenity")); QCOMPARE(pois[0].kind,QString("cafe"));
        QCOMPARE(pois[0].name,QString("测试咖啡馆")); QCOMPARE(pois[0].latitude,30.5); QCOMPARE(pois[0].longitude,114.3);
        QCOMPARE(pois[0].tags.size(),3);
        QCOMPARE(pois[1].osmType,1); QCOMPARE(pois[1].osmId,qint64(40)); QCOMPARE(pois[1].kind,QString("school"));
        QVERIFY(qAbs(pois[1].latitude-30.61)<1e-9); QVERIFY(qAbs(pois[1].longitude-114.41)<1e-9);
        QCOMPARE(data.pois({30.55,114.35,30.7,114.5,true}).size(),1);
        QCOMPARE(data.pois(everywhere,"shop").size(),0); QCOMPARE(data.pois(everywhere,"amenity",1).size(),1);
        QCOMPARE(data.findPois("学校").size(),1); QCOMPARE(data.findPois("测试").size(),2);
        QCOMPARE(data.findPois("%").size(),0);
        QVERIFY_EXCEPTION_THROWN(data.pois(everywhere,{},0),std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(data.findPois(" "),std::invalid_argument);

        // Roads: geometry, name and node identity survive for routing.
        const auto roads=data.roads({30.45,114.25,30.65,114.45,true});
        QCOMPARE(roads->wayCount(),quint32(1)); QCOMPARE(roads->nodeCount(),quint32(2));
        QCOMPARE(roads->displayName(*roads->wayIndex(10)),QString("测试路"));
        QVector<p1::data::MapRoad> mapRoads;
        p1::app::ImportResult entry=data.summary();
        p1::data::visitMapRoads(entry,{},[&](const auto& road){mapRoads.append(road);},{});
        QCOMPARE(mapRoads.size(),1); QCOMPARE(mapRoads[0].points,QVector<QPointF>({{114.3,30.5},{114.4,30.6}}));
        QCOMPARE(mapRoads[0].minZoom,12);

        // Areas: libosmium joined the two outer ways and attached the hole.
        QVector<p1::data::MapArea> areas;
        const auto areaStats=p1::data::visitMapAreas(entry,{},[&](const auto& area){areas.append(area);},{});
        QCOMPARE(areaStats.emitted,quint64(2)); QCOMPARE(areaStats.skipped,quint64(0));
        std::sort(areas.begin(),areas.end(),[](const auto& l,const auto& r){return int(l.kind)<int(r.kind);});
        QCOMPARE(areas[0].kind,p1::data::MapAreaKind::Water); QCOMPARE(areas[0].name,QString("测试湖"));
        QCOMPARE(areas[0].outer.size(),5); QCOMPARE(areas[0].outer.front(),areas[0].outer.back());
        QCOMPARE(areas[0].holes.size(),1); QCOMPARE(areas[0].holes[0].size(),5);
        QCOMPARE(areas[1].kind,p1::data::MapAreaKind::Building); QVERIFY(areas[1].holes.isEmpty());
        QCOMPARE(data.summary().sourceSha256,r.sourceSha256);
        QCOMPARE(data.summary().stats.tags,r.stats.tags);
        QCOMPARE(p1::data::listImportedDatasets(dir.filePath("cache")).size(),1);
    }
    void invalidInputs_data() {
        QTest::addColumn<QByteArray>("input"); QTest::addColumn<QString>("suffix");
        QTest::newRow("empty")<<QByteArray()<<QString(".osm");
        QTest::newRow("truncated")<<xml().left(60)<<QString(".osm");
        QTest::newRow("duplicate-id")<<QByteArray("<osm version=\"0.6\"><node id=\"1\" lat=\"30\" lon=\"114\"/><node id=\"1\" lat=\"31\" lon=\"115\"/></osm>")<<QString(".osm");
        QTest::newRow("latitude")<<QByteArray("<osm version=\"0.6\"><node id=\"1\" lat=\"91\" lon=\"114\"/></osm>")<<QString(".osm");
        QTest::newRow("no-location")<<QByteArray("<osm version=\"0.6\"><node id=\"1\"/></osm>")<<QString(".osm");
        QTest::newRow("deleted")<<QByteArray("<osm version=\"0.6\"><node id=\"1\" visible=\"false\"/></osm>")<<QString(".osm");
        QTest::newRow("broken-pbf")<<QByteArray("not a pbf")<<QString(".pbf");
        QTest::newRow("unsupported")<<xml()<<QString(".osmx");
    }
    void invalidInputs() {
        QFETCH(QByteArray,input); QFETCH(QString,suffix);
        QTemporaryDir dir; const auto source=dir.filePath("source"+suffix); write(source,input);
        p1::data::OsmImportParser parser(dir.filePath("cache"));
        const auto r=parser.parse(source,{},{},{});
        QVERIFY(!r.ok); QVERIFY(!r.error.isEmpty()); QVERIFY(r.cachePath.isEmpty());
        QCOMPARE(p1::data::listImportedDatasets(dir.filePath("cache")).size(),0);
    }
    void cancellationAndRollback() {
        QTemporaryDir dir; const auto source=dir.filePath("source.osm"); write(source,xml());
        p1::data::OsmImportParser parser(dir.filePath("cache"));
        const auto first=parser.parse(source,{},{},{}); QVERIFY(first.ok);
        const auto pre=parser.parse(source,{},{},[]{return true;}); QVERIFY(pre.cancelled);
        bool cancel=false;
        const auto mid=parser.parse(source,[&](int p,const QString&){if(p>=10) cancel=true;},{},[&]{return cancel;});
        QVERIFY(mid.cancelled); QVERIFY(!mid.ok);
        write(source,xml().left(50)); QVERIFY(!parser.parse(source,{},{},{}).ok);
        QCOMPARE(p1::data::listImportedDatasets(dir.filePath("cache")).size(),1);
        QCOMPARE(p1::data::ImportedDataset(first.cachePath).findPois("咖啡").size(),1);
        QVERIFY(QDir(dir.filePath("cache")).entryList({".import-*"},QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot).isEmpty());
    }
    void incompleteExtractIsReported() {
        QTemporaryDir dir; const auto source=dir.filePath("source.osm");
        // Road 10 misses node 999; multipolygon 50 misses way 77; way 60 crosses itself.
        write(source,"<osm version=\"0.6\">"
            "<node id=\"1\" lat=\"30.5\" lon=\"114.3\"/><node id=\"2\" lat=\"30.5\" lon=\"114.31\"/>"
            "<node id=\"3\" lat=\"30.51\" lon=\"114.3\"/><node id=\"4\" lat=\"30.51\" lon=\"114.31\"/>"
            "<way id=\"10\"><nd ref=\"1\"/><nd ref=\"999\"/><tag k=\"highway\" v=\"primary\"/></way>"
            "<way id=\"11\"><nd ref=\"1\"/><nd ref=\"2\"/><tag k=\"highway\" v=\"primary\"/></way>"
            "<way id=\"60\"><nd ref=\"1\"/><nd ref=\"2\"/><nd ref=\"3\"/><nd ref=\"4\"/><nd ref=\"1\"/><tag k=\"building\" v=\"yes\"/></way>"
            "<relation id=\"50\"><member type=\"way\" ref=\"77\" role=\"outer\"/><tag k=\"type\" v=\"multipolygon\"/><tag k=\"landuse\" v=\"forest\"/></relation></osm>");
        p1::data::OsmImportParser parser(dir.filePath("cache"));
        const auto r=parser.parse(source,{},{},{});
        QVERIFY2(r.ok,qPrintable(r.error)); QCOMPARE(r.warnings.size(),3);
        QCOMPARE(r.stats.roads,quint64(1)); QCOMPARE(r.stats.areas,quint64(0)); QCOMPARE(r.stats.areasSkipped,quint64(2));
        const p1::data::ImportedDataset data(r.cachePath);
        QCOMPARE(data.roads({30.4,114.2,30.6,114.4,true})->wayCount(),quint32(1));
        QCOMPARE(data.summary().warnings,r.warnings);
        QCOMPARE(p1::data::visitMapAreas(data.summary(),{},[](const auto&){},{}).skipped,quint64(2));
    }
    void unsortedInputIsRejected() {
        QTemporaryDir dir; const auto source=dir.filePath("source.osm");
        write(source,"<osm version=\"0.6\"><way id=\"10\"><nd ref=\"1\"/><nd ref=\"2\"/><tag k=\"highway\" v=\"primary\"/></way>"
            "<node id=\"1\" lat=\"30.5\" lon=\"114.3\"/><node id=\"2\" lat=\"30.5\" lon=\"114.31\"/></osm>");
        const auto r=p1::data::OsmImportParser(dir.filePath("cache")).parse(source,{},{},{});
        QVERIFY(!r.ok); QVERIFY(r.cachePath.isEmpty());
    }
    void oldRawCacheIsNotListed() {
        QTemporaryDir dir; QVERIFY(QDir().mkpath(dir.filePath("cache")));
        {
            p1::data::detail::Db db(dir.filePath("cache/old.sqlite"));
            db.exec("CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
                    "INSERT INTO metadata VALUES('schema','1'),('summary','{}');");
        }
        QCOMPARE(p1::data::listImportedDatasets(dir.filePath("cache")).size(),0);
        QVERIFY_EXCEPTION_THROWN(p1::data::ImportedDataset(dir.filePath("cache/old.sqlite")).summary(),std::runtime_error);
    }
};
QTEST_GUILESS_MAIN(ImportTests)
#include "ImportTests.moc"
