#include "app/MainWindow.h"
#include "app/ImportPage.h"
#include "app/NavigationWorkflow.h"
#include "data/import/OsmImportParser.h"
#include "contracts/ImportedDataset.h"
#include "algorithms/core/BoostAStar.h"
#include "ui/common/Theme.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QDoubleSpinBox>
#include <QTimer>
#include <QSet>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QXmlStreamWriter>
using namespace p1;
namespace {
QByteArray fixture(){return R"(<osm version="0.6">
<node id="1" lat="30.593" lon="114.305"/><node id="2" lat="30.593" lon="114.306"/>
<node id="3" lat="30.593" lon="114.307"/><node id="4" lat="30.594" lon="114.305"/><node id="5" lat="30.594" lon="114.306"/>
<node id="6" lat="31.5" lon="115.5"/><node id="7" lat="31.501" lon="115.501"/>
<way id="10"><nd ref="1"/><nd ref="2"/><nd ref="3"/><tag k="highway" v="residential"/><tag k="oneway" v="yes"/><tag k="name:zh" v="真实数据测试路"/></way>
<way id="11"><nd ref="4"/><nd ref="5"/><tag k="highway" v="residential"/></way>
<way id="12"><nd ref="6"/><nd ref="7"/><tag k="highway" v="primary"/></way></osm>)";}
void write(const QString& path,const QByteArray& b){QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(b)!=b.size())throw std::runtime_error("fixture write failed");}
}
class BusinessTests:public QObject{
    Q_OBJECT
private slots:
    void initTestCase(){QCoreApplication::setOrganizationName("P1Linux");QCoreApplication::setApplicationName("p1-navigation");}
    void init(){dir_=std::make_unique<QTemporaryDir>();source_=dir_->filePath("roads.osm");write(source_,fixture());
        imported_=data::OsmImportParser(dir_->filePath("cache")).parse(source_,{},{},{});QVERIFY2(imported_.ok,qPrintable(imported_.error));}
    void regionalRoads(){data::ImportedDataset cache(imported_.cachePath);auto ds=cache.roads({30.59,114.30,30.60,114.31,true});
        QCOMPARE(ds->wayCount(),quint32(2));QCOMPARE(ds->nodeCount(),quint32(5));QCOMPARE(ds->edges().size(),4);
        QCOMPARE(ds->displayName(*ds->wayIndex(10)),QString("真实数据测试路"));
        auto n=app::NavigationWorkflow::buildNetwork(ds);QCOMPARE(n->graph.edges.size(),size_t(4));
        auto path=algorithms::findRoute(n->graph,algorithms::model::NodeId(1),algorithms::model::NodeId(3));
        QCOMPARE(path.status,algorithms::RouteStatus::Success);QVERIFY(path.total_distance_m>180&&path.total_distance_m<200);
        QCOMPARE(algorithms::findRoute(n->graph,algorithms::model::NodeId(3),algorithms::model::NodeId(1)).status,algorithms::RouteStatus::Unreachable);
        auto c=std::make_shared<std::atomic_bool>(true);QVERIFY_EXCEPTION_THROWN(cache.roads({30.59,114.30,30.60,114.31,true},c),std::runtime_error);}
    void routeAndCancel(){app::NavigationWorkflow flow;QSignalSpy done(&flow,&app::NavigationWorkflow::completed);
        flow.load(imported_,30.593,114.305,1);QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,10000);QVERIFY(flow.state().network);
        flow.plan({114.305,30.593},{114.307,30.593});QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,10000);QVERIFY(done.last()[1].toBool());QCOMPARE(flow.state().route.size(),3);
        const auto prior=flow.state().route;flow.plan({114.307,30.593},{114.305,30.593});QTRY_COMPARE_WITH_TIMEOUT(done.count(),3,10000);QVERIFY(!done.last()[1].toBool());QCOMPARE(flow.state().route,prior);
        flow.plan({114.305,30.593},{114.305,30.594});QTRY_COMPARE_WITH_TIMEOUT(done.count(),4,10000);QVERIFY(!done.last()[1].toBool());
        flow.load(imported_,30.593,114.305,1);flow.cancel();QTest::qWait(200);QCOMPARE(done.count(),4);QVERIFY(!flow.busy());QCOMPARE(flow.state().route,prior);}
    void uiPipeline(){ui::Theme theme;theme.apply(*qApp);app::MainWindow window(theme,nullptr,dir_->filePath("ui-cache"));window.show();
        auto* import=window.findChild<app::ImportPage*>();auto* flow=window.findChild<app::NavigationWorkflow*>();QVERIFY(flow);
        QSignalSpy done(flow,&app::NavigationWorkflow::completed);int ticks=0;QTimer heartbeat;connect(&heartbeat,&QTimer::timeout,[&]{++ticks;});heartbeat.start(5);
        import->selectFile(source_);import->startImport();QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,15000);QVERIFY2(done[0][1].toBool(),qPrintable(flow->status()));
        QCOMPARE(window.currentPageId(),QString("map"));auto* canvas=window.findChild<QWidget*>("mapRenderCanvas");QTRY_VERIFY_WITH_TIMEOUT(canvas->property("realDataset").toBool(),10000);
        auto* map=window.findChild<QWidget*>("mapPage");map->findChild<QDoubleSpinBox*>("routeEndLatitude")->setValue(30.593);map->findChild<QDoubleSpinBox*>("routeEndLongitude")->setValue(114.307);
        map->findChild<QTabWidget*>()->setCurrentIndex(1);
        QTest::mouseClick(map->findChild<QPushButton*>("calculateRoute"),Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,10000);QVERIFY(done[1][1].toBool());
        QTRY_COMPARE_WITH_TIMEOUT(canvas->property("routeCount").toInt(),1,10000);QVERIFY(canvas->property("cacheHit").toBool());QVERIFY(ticks>2);QVERIFY(window.setCurrentPage("algorithms"));QVERIFY(window.findChild<QLabel*>("algorithmMetrics")->text().contains("耗时"));
        auto* algorithms=window.findChild<QWidget*>("algorithmsPage");algorithms->findChild<QTabWidget*>()->setCurrentIndex(1);
        QTest::mouseClick(algorithms->findChild<QPushButton*>("pickRouteStart"),Qt::LeftButton);QCOMPARE(window.currentPageId(),QString("map"));
        QTest::mouseClick(canvas,Qt::LeftButton,Qt::NoModifier,canvas->rect().center());
        QCOMPARE(map->findChild<QDoubleSpinBox*>("routeStartLongitude")->value(),algorithms->findChild<QDoubleSpinBox*>("routeStartLongitude")->value());}
    void malformedTrajectory(){app::NavigationWorkflow flow;QSignalSpy done(&flow,&app::NavigationWorkflow::completed);flow.load(imported_,30.593,114.305,1);QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,10000);
        const auto csv=dir_->filePath("bad.csv");write(csv,"lat,lon\n30.593,114.305\n");flow.match(csv);QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,10000);QVERIFY(!done.last()[1].toBool());QVERIFY(flow.state().matched.isEmpty());}
    void realHubei(){const auto cache=qEnvironmentVariable("P1_REAL_MAP_CACHE");if(cache.isEmpty())QSKIP("Set P1_REAL_MAP_CACHE for real Hubei acceptance");
        const auto imported=data::ImportedDataset(cache).summary();ui::Theme theme;theme.apply(*qApp);app::MainWindow window(theme);window.resize(1440,900);window.show();window.setCurrentPage("map");
        auto& flow=*window.findChild<app::NavigationWorkflow*>();QSignalSpy done(&flow,&app::NavigationWorkflow::completed);
        int ticks=0;QTimer heartbeat;connect(&heartbeat,&QTimer::timeout,[&]{++ticks;});heartbeat.start(10);
        flow.load(imported,30.593,114.305,1);QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,180000);QVERIFY2(done.last()[1].toBool(),qPrintable(flow.status()));qInfo().noquote()<<flow.status()<<flow.metrics();
        auto n=flow.state().network;QVector<quint32> walked;QSet<quint32> seen;quint32 at=0;
        for(quint32 i=0;i<n->roads->nodeCount();++i)if(n->roads->outgoingEdgeCount(i)){at=i;break;}
        walked.append(at);seen.insert(at);
        for(int step=0;step<20;++step){bool next=false;for(quint32 j=0;j<n->roads->outgoingEdgeCount(at);++j){const auto& e=n->roads->edgeAt(n->roads->outgoingEdgeFirst(at)+j);
                if(!seen.contains(e.to)){at=e.to;walked.append(at);seen.insert(at);next=true;break;}}if(!next)break;}
        QVERIFY(walked.size()>=3);const auto& a=n->roads->nodeAt(walked.first());const auto& b=n->roads->nodeAt(walked.last());
        flow.plan({a.lon,a.lat},{b.lon,b.lat});QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,30000);QVERIFY2(done.last()[1].toBool(),qPrintable(flow.status()));qInfo().noquote()<<flow.status()<<flow.metrics();
        QByteArray csv("latitude,longitude\n");int sample=0;for(auto pt:flow.state().route){if(sample++>=200)break;pt+=QPointF(0.000015,0.000015);
            csv+=QString("%1,%2\n").arg(pt.y(),0,'f',9).arg(pt.x(),0,'f',9).toUtf8();}
        const auto file=dir_->filePath("synthetic-gps-on-real-roads.csv");write(file,csv);flow.match(file);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(),3,240000);QVERIFY2(done.last()[1].toBool(),qPrintable(flow.status()));QVERIFY(flow.state().matched.size()>=2);QVERIFY(ticks>20);
        qInfo().noquote()<<"Synthetic GPS on actual Hubei road path; real FMM process:"<<flow.status()<<flow.metrics()<<"GUI heartbeat ticks"<<ticks;
        auto* canvas=window.findChild<QWidget*>("mapRenderCanvas");QTRY_COMPARE_WITH_TIMEOUT(canvas->property("routeCount").toInt(),3,10000);
        const auto captures=qEnvironmentVariable("P1_BUSINESS_CAPTURE_DIR");if(!captures.isEmpty()){
            QTest::mouseClick(window.findChild<QPushButton*>("fitNavigationResult"),Qt::LeftButton);QTest::qWait(400);
            QVERIFY(QDir().mkpath(captures));QVERIFY(window.grab().save(QDir(captures).filePath("hubei-route-match-light.png")));
            theme.setDark(true);QTest::qWait(500);QVERIFY(window.grab().save(QDir(captures).filePath("hubei-route-match-dark.png")));
            window.resize(1024,720);QTest::qWait(500);QVERIFY(window.grab().save(QDir(captures).filePath("hubei-compact.png")));
        }
    }
    void pairedScenario(){const auto cache=qEnvironmentVariable("P1_REAL_MAP_CACHE");if(cache.isEmpty())QSKIP("real map cache required");
        ui::Theme theme;theme.apply(*qApp);app::MainWindow window(theme);window.show();window.setCurrentPage("map");auto& flow=*window.findChild<app::NavigationWorkflow*>();
        QSignalSpy done(&flow,&app::NavigationWorkflow::completed);flow.load(data::ImportedDataset(cache).summary(),30.593,114.305,1);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,180000);QVERIFY2(done.last()[1].toBool(),qPrintable(flow.status()));
        QJsonArray samples;
        const auto evidence=qEnvironmentVariable("P1_BUSINESS_EVIDENCE_DIR");
        if(!evidence.isEmpty()){
            const auto roads=flow.state().network->roads;QFile f(QDir(evidence).filePath("shared-region.osm"));QVERIFY(f.open(QIODevice::WriteOnly));
            QXmlStreamWriter xml(&f);xml.setAutoFormatting(true);xml.writeStartDocument();xml.writeStartElement("osm");xml.writeAttribute("version","0.6");xml.writeAttribute("generator","P1 shared public road snapshot");
            for(quint32 i=0;i<roads->nodeCount();++i){const auto& n=roads->nodeAt(i);xml.writeStartElement("node");xml.writeAttribute("id",QString::number(n.id));
                xml.writeAttribute("lat",QString::number(n.lat,'g',17));xml.writeAttribute("lon",QString::number(n.lon,'g',17));xml.writeEndElement();}
            for(quint32 i=0;i<roads->wayCount();++i){const auto& way=roads->wayAt(i);xml.writeStartElement("way");xml.writeAttribute("id",QString::number(way.id));
                for(auto id:roads->wayNodeIds(i)){xml.writeStartElement("nd");xml.writeAttribute("ref",QString::number(id));xml.writeEndElement();}
                for(const auto& t:roads->wayTags(i)){xml.writeStartElement("tag");xml.writeAttribute("k",roads->stringAt(t.key));xml.writeAttribute("v",roads->stringAt(t.value));xml.writeEndElement();}
                xml.writeEndElement();}
            xml.writeEndElement();xml.writeEndDocument();QVERIFY(!xml.hasError());f.close();
        }
        QPointF start,end;bool scenario=false;const auto ds=flow.state().network->roads;
        for(quint32 wi=0;wi<ds->wayCount();++wi){const auto& way=ds->wayAt(wi);
            if(!way.attributes.isRoutable()||way.refCount<3||way.attributes.roadClass!=data::RoadClass::Residential)continue;
            const auto ni=ds->wayNodePool().at(int(way.refFirst)),nj=ds->wayNodePool().at(int(way.refFirst+way.refCount-1));
            const auto& a=ds->nodeAt(ni);const auto& b=ds->nodeAt(nj);
            if(ni==nj||ds->nearestNode(a.lat,a.lon)!=ni||ds->nearestNode(b.lat,b.lon)!=nj)continue;
            start={a.lon,a.lat};end={b.lon,b.lat};if(way.attributes.isOnewayReversed())std::swap(start,end);scenario=true;break;}
        QVERIFY(scenario);
        for(int i=-1;i<10;++i){QEventLoop loop;QElapsedTimer clock;double elapsed=0;bool ok=false;
            auto connection=connect(&flow,&app::NavigationWorkflow::completed,&loop,[&](const QString&,bool success){elapsed=clock.nsecsElapsed()/1e6;ok=success;loop.quit();});
            QTimer::singleShot(30000,&loop,&QEventLoop::quit);clock.start();flow.plan(start,end);loop.exec();disconnect(connection);
            QVERIFY2(ok,qPrintable(flow.status()));if(i>=0)samples.append(QJsonObject{{"request_to_result_ms",elapsed},{"worker_ms",flow.lastResult().elapsedMs},
                {"algorithm_ms",flow.lastResult().algorithmMs},{"distance_m",flow.state().distanceM},{"edges",flow.lastResult().count}});}
        auto* map=window.findChild<QWidget*>("mapPage");auto* canvas=window.findChild<QWidget*>("mapRenderCanvas");
        QTRY_VERIFY_WITH_TIMEOUT(canvas->property("realDataset").toBool(),10000);
        QJsonArray redraws;
        for(int i=-1;i<10;++i){QEventLoop loop;QElapsedTimer clock;
            auto c=connect(map,SIGNAL(framePublished()),&loop,SLOT(quit()));
            QTimer::singleShot(10000,&loop,&QEventLoop::quit);clock.start();
            QTest::mouseClick(window.findChild<QPushButton*>("fitRoadNetwork"),Qt::LeftButton);loop.exec();disconnect(c);
            if(i>=0)redraws.append(QJsonObject{{"request_to_frame_ms",clock.nsecsElapsed()/1e6},
                {"worker_ms",canvas->property("renderElapsedMs").toDouble()},{"base_cache_hit",canvas->property("cacheHit").toBool()}});}
        const auto network=flow.state().network;const auto bounds=network->roads->bounds();
        double width=(bounds.maxLon-bounds.minLon)*network->metresLon*1.1;
        double height=(bounds.maxLat-bounds.minLat)*network->metresLat*1.1;
        const double aspect=double(canvas->width())/canvas->height();width=std::max(width,height*aspect);height=width/aspect;
        const QJsonObject viewport{{"width_px",canvas->width()},{"height_px",canvas->height()},
            {"width_m",width},{"height_m",height},{"center_lon",bounds.centerLon()},{"center_lat",bounds.centerLat()}};
        const QJsonObject output{{"input","Hubei PBF b099af20e3e02e13a68ec958ae2a5f05ec28f21664dede7eb33f76b20eca0ffe"},
            {"warmups",1},{"repeats",10},{"start",QJsonArray{start.x(),start.y()}},{"end",QJsonArray{end.x(),end.y()}},
            {"policy","regional shortest distance, node snap <=500m, no turn restrictions"},{"samples",samples},{"redraws",redraws},{"viewport",viewport}};
        qInfo().noquote()<<"P1_ROUTE_MEASUREMENTS"<<QJsonDocument(output).toJson(QJsonDocument::Compact);
    }
private:std::unique_ptr<QTemporaryDir> dir_;QString source_;app::ImportResult imported_;
};
QTEST_MAIN(BusinessTests)
#include "BusinessTests.moc"
