#include "app/MainWindow.h"
#include "app/ImportPage.h"
#include "app/NavigationWorkflow.h"
#include "app/RoutingProfile.h"
#include "data/import/PoiSearch.h"
#include <QLineEdit>
#include <QLabel>
#include "data/import/OsmImportParser.h"
#include "contracts/ImportedDataset.h"
#include "algorithms/core/BoostAStar.h"
#include "ui/common/Theme.h"
#include "ui/map/MapRegionLoader.h"
#include "ui/map/TilePyramid.h"
#include <QtTest>
#include <QDirIterator>
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
#include <QAction>
#include <QContextMenuEvent>
#include <QMenu>
#include <QPointer>
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
QPoint mapPixel(QWidget* canvas,const app::RoadNetwork& projection,QPointF point) {
    Q_UNUSED(projection);
    const auto center=ui::mapProject({canvas->property("centerLongitude").toDouble(),canvas->property("centerLatitude").toDouble()});
    const auto p=ui::mapProject(point);
    return {qRound((.5+(p.x()-center.x())/canvas->property("viewWidthMetres").toDouble())*canvas->width()),
            qRound((.5-(p.y()-center.y())/canvas->property("viewHeightMetres").toDouble())*canvas->height())};
}
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
        auto* map=window.findChild<QWidget*>("mapPage");QVERIFY(!map->findChild<QDoubleSpinBox*>());
        QTRY_VERIFY_WITH_TIMEOUT(canvas->property("regionRevision").toULongLong()>1&&canvas->property("tilesComplete").toBool(),10000);
        auto choose=[&](bool start,QPointF geo) {
            const auto point=mapPixel(canvas,*flow->state().network,geo);
            QContextMenuEvent event(QContextMenuEvent::Mouse,point,canvas->mapToGlobal(point));QApplication::sendEvent(canvas,&event);
            auto* menu=canvas->findChild<QMenu*>("mapPointMenu");if(!menu) return false;
            auto* action=menu->findChild<QAction*>(start?"setMapStart":"setMapEnd");if(!action) return false;
            action->trigger();menu->close();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);return true;
        };
        QVERIFY(choose(true,{114.305,30.593}));QVERIFY(flow->startPoint());
        QVERIFY(std::abs(flow->startPoint()->x()-114.305)<.00001);
        QVERIFY(choose(false,{114.307,30.593}));QVERIFY(flow->endPoint());
        QTRY_COMPARE_WITH_TIMEOUT(canvas->property("markerCount").toInt(),2,10000);
        QTest::mouseClick(map->findChild<QPushButton*>("calculateRoute"),Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,10000);QVERIFY(done[1][1].toBool());
        QTRY_COMPARE_WITH_TIMEOUT(canvas->property("routeCount").toInt(),1,10000);QVERIFY(canvas->property("cacheHit").toBool());QVERIFY(ticks>2);QVERIFY(window.setCurrentPage("algorithms"));QVERIFY(window.findChild<QLabel*>("algorithmMetrics")->text().contains("耗时"));
        auto* algorithms=window.findChild<QWidget*>("algorithmsPage");algorithms->findChild<QTabWidget*>()->setCurrentIndex(1);
        QTest::mouseClick(algorithms->findChild<QPushButton*>("pickRouteStart"),Qt::LeftButton);QCOMPARE(window.currentPageId(),QString("map"));
        QTest::mouseClick(canvas,Qt::LeftButton,Qt::NoModifier,canvas->rect().center());
        QVERIFY(std::abs(flow->startPoint()->x()-algorithms->findChild<QDoubleSpinBox*>("routeStartLongitude")->value())<.0000001);}
    void automaticMapRegions(){
        ui::Theme theme;theme.apply(*qApp);app::MainWindow window(theme,nullptr,dir_->filePath("cache"));window.show();window.setCurrentPage("map");
        auto* flow=window.findChild<app::NavigationWorkflow*>();QSignalSpy done(flow,&app::NavigationWorkflow::completed);
        flow->load(imported_,30.593,114.305,1);QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,10000);
        auto* canvas=window.findChild<QWidget*>("mapRenderCanvas");QTRY_VERIFY_WITH_TIMEOUT(canvas->property("regionRevision").toULongLong()>1&&canvas->property("tilesComplete").toBool(),10000);
        flow->plan({114.305,30.593},{114.307,30.593});QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,10000);
        const auto originalRoute=flow->state().route;const auto originalStart=flow->startPoint();
        const double x=(ui::mapProject({115.5005,31.5005}).x()-ui::mapProject({canvas->property("centerLongitude").toDouble(),canvas->property("centerLatitude").toDouble()}).x())/canvas->property("viewWidthMetres").toDouble();
        const double y=(ui::mapProject({115.5005,31.5005}).y()-ui::mapProject({canvas->property("centerLongitude").toDouble(),canvas->property("centerLatitude").toDouble()}).y())/canvas->property("viewHeightMetres").toDouble();
        const auto revision=canvas->property("regionRevision").toULongLong();
        QVERIFY(QMetaObject::invokeMethod(canvas,"panned",Q_ARG(QPointF,QPointF(x,y))));
        // Progressive frames come first; the newly requested view settles on complete.
        QTRY_VERIFY_WITH_TIMEOUT(canvas->property("regionRevision").toULongLong()>revision&&canvas->property("tilesComplete").toBool()&&canvas->property("roadCount").toUInt()==1,10000);
        QVERIFY(std::abs(canvas->property("centerLongitude").toDouble()-115.5005)<.00001);
        QCOMPARE(flow->state().route,originalRoute);QVERIFY(flow->startPoint()==originalStart);
        // Planning on roads discovered by browsing loads the endpoints' region.
        flow->plan({115.5,31.5},{115.501,31.501});QTRY_COMPARE_WITH_TIMEOUT(done.count(),3,10000);
        QVERIFY2(done.last()[1].toBool(),qPrintable(flow->status()));
        QVERIFY(flow->state().route.front().x()>115.49);
        const auto after=canvas->property("regionRevision").toULongLong();
        QVERIFY(QMetaObject::invokeMethod(canvas,"panned",Q_ARG(QPointF,QPointF(20,0))));
        QTRY_VERIFY_WITH_TIMEOUT(canvas->property("regionRevision").toULongLong()>after&&canvas->property("tilesComplete").toBool()&&canvas->property("roadCount").toUInt()==0,10000);
        QVERIFY(canvas->property("tileCount").toInt()>0);
    }
    void clarityMenu(){
        ui::Theme theme;theme.apply(*qApp);app::MainWindow window(theme,nullptr,dir_->filePath("cache"));window.show();window.setCurrentPage("map");
        auto* flow=window.findChild<app::NavigationWorkflow*>();QSignalSpy done(flow,&app::NavigationWorkflow::completed);
        flow->load(imported_,30.593,114.305,1);QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,10000);
        auto* canvas=window.findChild<QWidget*>("mapRenderCanvas");
        QTRY_VERIFY_WITH_TIMEOUT(canvas->property("regionRevision").toULongLong()>1&&canvas->property("tilesComplete").toBool(),10000);
        auto* high=window.findChild<QAction*>("mapClarityHigh");auto* medium=window.findChild<QAction*>("mapClarityMedium");auto* low=window.findChild<QAction*>("mapClarityLow");
        QVERIFY(high&&medium&&low);QVERIFY(low->isChecked()&&!high->isChecked()&&!medium->isChecked());
        QCOMPARE(window.findChild<QMenu*>("mapClarityMenu")->title(),QString("地图清晰度"));
        const auto zoom=canvas->property("tileZoom").toInt();const auto revision=canvas->property("regionRevision").toULongLong();
        high->trigger();QVERIFY(high->isChecked()&&!low->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(canvas->property("regionRevision").toULongLong()>revision&&canvas->property("tilesComplete").toBool(),10000);
        QCOMPARE(canvas->property("tileZoom").toInt(),zoom);
        QDirIterator it(dir_->filePath("cache"),{"*.png"},QDir::Files,QDirIterator::Subdirectories);bool dense=false;
        while(it.hasNext())if(it.next().contains("-d512/"))dense=true;
        QVERIFY(dense);
    }
    void drivingModeExcludesNonCarRoads(){
        // A--B: footway 96 m; A-E-B: residential with motorcar=no ~100 m;
        // A-C-D-B: residential detour ~318 m. General takes the footway,
        // driving must take the detour (Navit car profile semantics).
        const QByteArray osm=R"(<osm version="0.6">
<node id="1" lat="30.5900" lon="114.3000"/><node id="2" lat="30.5900" lon="114.3010"/>
<node id="5" lat="30.5899" lon="114.3005"/>
<node id="6" lat="30.5910" lon="114.3000"/><node id="7" lat="30.5910" lon="114.3010"/>
<node id="8" lat="30.5920" lon="114.3000"/><node id="9" lat="30.5920" lon="114.3010"/>
<way id="20"><nd ref="1"/><nd ref="2"/><tag k="highway" v="footway"/></way>
<way id="21"><nd ref="1"/><nd ref="5"/><nd ref="2"/><tag k="highway" v="residential"/><tag k="motorcar" v="no"/></way>
<way id="22"><nd ref="1"/><nd ref="6"/><nd ref="7"/><nd ref="2"/><tag k="highway" v="residential"/></way>
<way id="30"><nd ref="6"/><nd ref="8"/><tag k="highway" v="track"/><tag k="tracktype" v="grade5"/></way>
<way id="31"><nd ref="7"/><nd ref="9"/><tag k="highway" v="track"/><tag k="tracktype" v="grade2"/></way>
<way id="32"><nd ref="8"/><nd ref="9"/><tag k="highway" v="pedestrian"/></way>
<way id="33"><nd ref="8"/><nd ref="6"/><tag k="highway" v="living_street"/><tag k="vehicle" v="destination"/></way>
<way id="34"><nd ref="9"/><nd ref="7"/><tag k="highway" v="service"/><tag k="access" v="no"/><tag k="motorcar" v="yes"/></way>
<way id="35"><nd ref="8"/><nd ref="7"/><tag k="highway" v="living_street"/></way>
<way id="36"><nd ref="6"/><nd ref="9"/><tag k="highway" v="steps"/></way></osm>)";
        const auto file=dir_->filePath("driving.osm");write(file,osm);
        const auto entry=data::OsmImportParser(dir_->filePath("driving-cache")).parse(file,{},{},{});QVERIFY2(entry.ok,qPrintable(entry.error));
        const auto ds=data::ImportedDataset(entry.cachePath).roads({30.58,114.29,30.60,114.31,true});
        auto allowed=[&](qint64 id){return app::carAllowed(*ds,*ds->wayIndex(id));};
        QVERIFY(!allowed(20));QVERIFY(!allowed(21));QVERIFY(allowed(22));QVERIFY(!allowed(30));QVERIFY(allowed(31));
        QVERIFY(!allowed(32));QVERIFY(!allowed(33));QVERIFY(allowed(35));QVERIFY(!allowed(36));
        // access=no already makes the way non-routable at import, so a later motorcar=yes cannot add it back.
        QCOMPARE(allowed(34),ds->wayAt(*ds->wayIndex(34)).attributes.isRoutable());
        app::NavigationWorkflow flow;QSignalSpy done(&flow,&app::NavigationWorkflow::completed);
        flow.load(entry,30.5905,114.3005,1);QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,10000);QVERIFY2(done.last()[1].toBool(),qPrintable(flow.status()));
        QCOMPARE(flow.routeMode(),app::RouteMode::General);
        flow.plan({114.3000,30.5900},{114.3010,30.5900});QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,10000);QVERIFY2(done.last()[1].toBool(),qPrintable(flow.status()));
        const double general=flow.state().distanceM;QVERIFY2(general>90&&general<100,qPrintable(QString::number(general)));
        flow.setRouteMode(app::RouteMode::Driving);QCOMPARE(flow.routeMode(),app::RouteMode::Driving);
        flow.plan({114.3000,30.5900},{114.3010,30.5900});QTRY_COMPARE_WITH_TIMEOUT(done.count(),3,10000);QVERIFY2(done.last()[1].toBool(),qPrintable(flow.status()));
        const double driving=flow.state().distanceM;QVERIFY2(driving>300&&driving<340,qPrintable(QString::number(driving)));
        QVERIFY(flow.status().contains("行车模式"));
        bool detour=false;for(auto p:flow.state().route)if(std::abs(p.y()-30.5910)<1e-6)detour=true;QVERIFY(detour);
        // An endpoint only on the footway's middle snaps to the nearest car-road node.
        flow.plan({114.30005,30.59},{114.3010,30.5910});QTRY_COMPARE_WITH_TIMEOUT(done.count(),4,10000);QVERIFY2(done.last()[1].toBool(),qPrintable(flow.status()));
        QCOMPARE(flow.state().route.front(),QPointF(114.3000,30.5900));
    }
    void routeModeMenu(){
        ui::Theme theme;theme.apply(*qApp);app::MainWindow window(theme,nullptr,dir_->filePath("cache"));window.show();window.setCurrentPage("map");
        auto* flow=window.findChild<app::NavigationWorkflow*>();QSignalSpy done(flow,&app::NavigationWorkflow::completed);
        flow->load(imported_,30.593,114.305,1);QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,10000);
        QCOMPARE(window.findChild<QMenu*>("mapRouteModeMenu")->title(),QString("路线模式"));
        auto* general=window.findChild<QAction*>("routeModeGeneral");auto* driving=window.findChild<QAction*>("routeModeDriving");QVERIFY(general&&driving);
        QCOMPARE(general->text(),QString("一般模式"));QCOMPARE(driving->text(),QString("行车模式"));
        QVERIFY(general->isChecked()&&!driving->isChecked());
        flow->setEndpoint(true,{114.305,30.593});flow->setEndpoint(false,{114.307,30.593});
        driving->trigger();QVERIFY(driving->isChecked()&&!general->isChecked());QCOMPARE(flow->routeMode(),app::RouteMode::Driving);
        // Switching with both endpoints set re-plans under the new mode.
        QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,10000);QVERIFY2(done.last()[1].toBool(),qPrintable(flow->status()));QVERIFY(flow->status().contains("行车模式"));
        general->trigger();QCOMPARE(flow->routeMode(),app::RouteMode::General);QTRY_COMPARE_WITH_TIMEOUT(done.count(),3,10000);QVERIFY(flow->status().contains("一般模式"));
    }
    void poiSearchGoHere(){
        // Roads of the shared fixture plus 25 POIs "测试超市i" spaced 0.0001° east along 30.5932.
        // POI nodes go before the first way: the importer requires nodes, ways, relations order.
        QByteArray nodes;
        for(int i=0;i<25;++i)nodes+=QString("<node id=\"%1\" lat=\"30.5932\" lon=\"%2\"><tag k=\"shop\" v=\"supermarket\"/><tag k=\"name\" v=\"测试超市%3\"/></node>").arg(900+i).arg(114.305+i*0.0001,0,'f',7).arg(i).toUtf8();
        nodes+="<node id=\"990\" lat=\"30.5935\" lon=\"114.3055\"><tag k=\"amenity\" v=\"cafe\"/><tag k=\"name\" v=\"其他咖啡\"/></node>";
        QByteArray osm=fixture();osm.insert(osm.indexOf("<way"),nodes);
        const auto file=dir_->filePath("poi.osm");write(file,osm);
        const auto entry=data::OsmImportParser(dir_->filePath("poi-cache")).parse(file,{},{},{});QVERIFY2(entry.ok,qPrintable(entry.error));
        ui::Theme theme;theme.apply(*qApp);app::MainWindow window(theme,nullptr,dir_->filePath("poi-cache"));window.resize(1200,800);window.show();window.setCurrentPage("map");
        auto* flow=window.findChild<app::NavigationWorkflow*>();QSignalSpy done(flow,&app::NavigationWorkflow::completed);
        flow->load(entry,30.593,114.305,1);QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,10000);
        auto* canvas=window.findChild<QWidget*>("mapRenderCanvas");auto* map=window.findChild<QWidget*>("mapPage");
        QTRY_VERIFY_WITH_TIMEOUT(canvas->property("regionRevision").toULongLong()>1&&canvas->property("tilesComplete").toBool(),10000);
        auto* button=window.findChild<QPushButton*>("poiSearchButton");auto* bar=window.findChild<QWidget*>("poiSearchBar");auto* input=window.findChild<QLineEdit*>("poiSearchInput");
        QVERIFY(button&&bar&&input);QVERIFY(button->isVisible()&&!bar->isVisible());
        QVERIFY(button->mapTo(map,QPoint(0,0)).x()>map->width()/2&&button->mapTo(map,QPoint(0,0)).y()<80); // top-right
        button->click();QVERIFY(!bar->isVisible());QVERIFY(canvas->property("mapNotice").toString().contains("起点"));
        const QPointF start(114.305,30.5925);flow->setEndpoint(true,start);
        button->click();QVERIFY(bar->isVisible()&&!button->isVisible());
        const auto barCentre=bar->mapTo(map,QPoint(bar->width()/2,0));QVERIFY(std::abs(barCentre.x()-map->width()/2)<40);QVERIFY(barCentre.y()<80);
        QSignalSpy results(map,SIGNAL(poiResultsReady()));
        input->setText("超市");QMetaObject::invokeMethod(input,"returnPressed");
        QTRY_COMPARE_WITH_TIMEOUT(results.count(),1,10000);QCOMPARE(canvas->property("poiCount").toInt(),20);
        QVERIFY(canvas->property("mapNotice").toString().contains("最近的 20"));
        const auto expected=data::searchNearestPoisByScan(entry.cachePath,"超市",start.y(),start.x(),20);QCOMPARE(expected.size(),20);
        QCOMPARE(expected.last().poi.name,QString("测试超市19"));
        QTest::qWait(600); // view fitted to the results; let the frame settle
        const auto target=QPointF(expected.last().poi.longitude,expected.last().poi.latitude);
        const auto at=mapPixel(canvas,*flow->state().network,target);
        QVERIFY(QMetaObject::invokeMethod(canvas,"clicked",Q_ARG(QPointF,QPointF(double(at.x())/canvas->width(),double(at.y())/canvas->height()))));
        auto* info=window.findChild<QLabel*>("poiInfo");QVERIFY(info&&info->isVisible());QVERIFY(info->text().contains("测试超市19"));
        const auto captures=qEnvironmentVariable("P1_BUSINESS_CAPTURE_DIR");
        if(!captures.isEmpty()){QVERIFY(QDir().mkpath(captures));QVERIFY(window.grab().save(QDir(captures).filePath("poi-search-info.png")));}
        {   // The westernmost result lies inside the 800 px offscreen screen: the menu opens beside it.
            const auto west=mapPixel(canvas,*flow->state().network,{expected.first().poi.longitude,expected.first().poi.latitude});
            QVERIFY(canvas->mapToGlobal(west).x()<600);
            QVERIFY(QMetaObject::invokeMethod(canvas,"menuRequested",Q_ARG(QPoint,west)));
            QMenu* first=nullptr;QTRY_VERIFY_WITH_TIMEOUT((first=window.findChild<QMenu*>("poiMenu"))&&first->isVisible(),3000);
            QVERIFY2(QLineF(first->pos(),canvas->mapToGlobal(west)).length()<30,qPrintable(QString("%1,%2").arg(first->pos().x()).arg(first->pos().y())));
            first->close();QTRY_VERIFY_WITH_TIMEOUT(!window.findChild<QMenu*>("poiMenu"),3000);
        }
        QVERIFY(QMetaObject::invokeMethod(canvas,"menuRequested",Q_ARG(QPoint,at)));
        QMenu* menu=nullptr;QTRY_VERIFY_WITH_TIMEOUT((menu=window.findChild<QMenu*>("poiMenu"))&&menu->isVisible(),3000);
        auto* go=menu->findChild<QAction*>("poiGoHere");QVERIFY(go);QCOMPARE(go->text(),QString("到这去"));
        // Opens next to the point. Qt keeps popups on screen, so only check when
        // the point lies well inside the (800 px wide) offscreen test screen.
        if(menu->screen()->geometry().adjusted(0,0,-menu->width(),-menu->height()).contains(canvas->mapToGlobal(at)))
            QVERIFY(QLineF(menu->pos(),canvas->mapToGlobal(at)).length()<60);
        go->trigger();menu->close();
        QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,10000);QVERIFY2(done.last()[1].toBool(),qPrintable(flow->status()));
        QCOMPARE(*flow->endPoint(),target);QVERIFY(flow->state().route.size()>=2);
        // Dashed connector from the picked start to the snapped road node.
        QCOMPARE(flow->state().route.front(),QPointF(114.305,30.593));
        QTRY_COMPARE_WITH_TIMEOUT(canvas->property("routeCount").toInt(),1,5000);QTest::qWait(300);
        const auto image=canvas->grab().toImage();const auto a=mapPixel(canvas,*flow->state().network,start),b=mapPixel(canvas,*flow->state().network,flow->state().route.front());
        const auto route=ui::mapColors(false).route;int dash=0;
        for(int k=20;k<=80;++k){const auto p=QLineF(a,b).pointAt(k/100.).toPoint();const auto c=image.pixelColor(p);
            if(std::abs(c.red()-route.red())+std::abs(c.green()-route.green())+std::abs(c.blue()-route.blue())<90)++dash;}
        QVERIFY2(dash>5&&dash<55,qPrintable(QString::number(dash))); // both inked and gapped: dashed
        if(!captures.isEmpty())QVERIFY(window.grab().save(QDir(captures).filePath("poi-search-route.png")));
        QWidget* close=window.findChild<QWidget*>("poiSearchClose");QMetaObject::invokeMethod(close,"click");
        QVERIFY(!bar->isVisible()&&button->isVisible());QCOMPARE(canvas->property("poiCount").toInt(),0);
    }
    void planningExpandsAreaWhenRouteLeavesIt(){
        // A and B (~960 m apart) connect only via a detour 1.5 km north: the
        // northern way lies wholly outside the 1 km planning margin (ways that
        // cross the margin are loaded whole). D-E is an isolated road east of B.
        const QByteArray osm=R"(<osm version="0.6">
<node id="1" lat="30.5000" lon="114.3000"/><node id="2" lat="30.5000" lon="114.3100"/>
<node id="3" lat="30.5135" lon="114.3000"/><node id="4" lat="30.5135" lon="114.3100"/>
<node id="5" lat="30.5000" lon="114.3200"/><node id="6" lat="30.5000" lon="114.3220"/>
<way id="1"><nd ref="1"/><nd ref="3"/><tag k="highway" v="residential"/></way>
<way id="2"><nd ref="3"/><nd ref="4"/><tag k="highway" v="residential"/></way>
<way id="3"><nd ref="4"/><nd ref="2"/><tag k="highway" v="residential"/></way>
<way id="4"><nd ref="5"/><nd ref="6"/><tag k="highway" v="residential"/></way></osm>)";
        const auto file=dir_->filePath("detour.osm");write(file,osm);
        const auto entry=data::OsmImportParser(dir_->filePath("detour-cache")).parse(file,{},{},{});QVERIFY2(entry.ok,qPrintable(entry.error));
        app::NavigationWorkflow flow;QSignalSpy done(&flow,&app::NavigationWorkflow::completed);
        flow.load(entry,30.5,114.3,0.2);QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,10000);QVERIFY2(done.last()[1].toBool(),qPrintable(flow.status()));
        {   // The old fixed 1 km margin alone has no path.
            const double dy=1000./111195.08,dx=dy/std::cos(30.5*std::acos(-1.)/180);
            auto ds=data::ImportedDataset(entry.cachePath).roads({30.5-dy,114.30-dx,30.5+dy,114.31+dx,true});auto n=app::NavigationWorkflow::buildNetwork(ds);
            QCOMPARE(algorithms::findRoute(n->graph,algorithms::model::NodeId(*ds->nearestNode(30.5,114.30,10)+1),algorithms::model::NodeId(*ds->nearestNode(30.5,114.31,10)+1)).status,algorithms::RouteStatus::Unreachable);
        }
        flow.plan({114.30,30.5},{114.31,30.5});QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,10000);
        QVERIFY2(done.last()[1].toBool(),qPrintable(flow.status()));
        QVERIFY(flow.state().distanceM>3900&&flow.state().distanceM<4100);
        QVERIFY2(flow.status().contains("自动扩大到起终点外 2 km"),qPrintable(flow.status()));
        bool north=false;for(auto p:flow.state().route)if(p.y()>30.513)north=true;QVERIFY(north);
        flow.plan({114.30,30.5},{114.32,30.5});QTRY_COMPARE_WITH_TIMEOUT(done.count(),3,10000);
        QVERIFY(!done.last()[1].toBool());
        QVERIFY2(flow.status().contains("无法规划")&&flow.status().contains("8 km 仍不连通"),qPrintable(flow.status()));
    }
    void longRouteUsesTieredNetwork(){
        // 0.7° of longitude (~67 km): over the full-detail limit. A-B link via a
        // motorway; C-D only via residential ways whose middle lies outside every tier.
        const QByteArray osm=R"(<osm version="0.6">
<node id="1" lat="30.5000" lon="114.0000"/><node id="2" lat="30.5000" lon="114.0100"/>
<node id="3" lat="30.5000" lon="114.3500"/><node id="4" lat="30.5000" lon="114.6900"/><node id="5" lat="30.5000" lon="114.7000"/>
<node id="11" lat="30.7000" lon="114.0000"/><node id="12" lat="30.7000" lon="114.0100"/>
<node id="13" lat="30.7000" lon="114.2000"/><node id="14" lat="30.7000" lon="114.5000"/>
<node id="15" lat="30.7000" lon="114.6900"/><node id="16" lat="30.7000" lon="114.7000"/>
<way id="1"><nd ref="1"/><nd ref="2"/><tag k="highway" v="residential"/></way>
<way id="2"><nd ref="2"/><nd ref="3"/><nd ref="4"/><tag k="highway" v="motorway"/><tag k="oneway" v="no"/></way>
<way id="3"><nd ref="4"/><nd ref="5"/><tag k="highway" v="residential"/></way>
<way id="11"><nd ref="11"/><nd ref="12"/><nd ref="13"/><tag k="highway" v="residential"/></way>
<way id="12"><nd ref="13"/><nd ref="14"/><tag k="highway" v="residential"/></way>
<way id="13"><nd ref="14"/><nd ref="15"/><nd ref="16"/><tag k="highway" v="residential"/></way></osm>)";
        const auto file=dir_->filePath("long.osm");write(file,osm);
        const auto entry=data::OsmImportParser(dir_->filePath("long-cache")).parse(file,{},{},{});QVERIFY2(entry.ok,qPrintable(entry.error));
        QVERIFY_EXCEPTION_THROWN(data::ImportedDataset(entry.cachePath).roads({30.49,113.99,30.51,114.71,true}),std::invalid_argument);
        app::NavigationWorkflow flow;QSignalSpy done(&flow,&app::NavigationWorkflow::completed);
        flow.load(entry,30.5,114.0,1);QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,10000);QVERIFY2(done.last()[1].toBool(),qPrintable(flow.status()));
        for(auto mode:{app::RouteMode::General,app::RouteMode::Driving}){
            flow.setRouteMode(mode);const int before=done.count();
            flow.plan({114.0,30.5},{114.7,30.5});QTRY_COMPARE_WITH_TIMEOUT(done.count(),before+1,20000);
            QVERIFY2(done.last()[1].toBool(),qPrintable(flow.status()));QVERIFY2(flow.status().contains("分级加载"),qPrintable(flow.status()));
            QVERIFY(flow.state().distanceM>66000&&flow.state().distanceM<69000);
        }
        flow.setRouteMode(app::RouteMode::General);const int before=done.count();
        flow.plan({114.0,30.7},{114.7,30.7});QTRY_COMPARE_WITH_TIMEOUT(done.count(),before+1,20000);
        QVERIFY(!done.last()[1].toBool());
        QVERIFY2(flow.status().contains("无法规划")&&flow.status().contains("已尝试分级路网"),qPrintable(flow.status()));
        // Layered query: filtered layers have no area cap; an all-roads layer still does.
        data::ImportedDataset cache(entry.cachePath);
        QCOMPARE(cache.roadsLayered({{{30.4,113.9,30.8,114.8,true},{"motorway"}}})->wayCount(),quint32(1));
        QVERIFY_EXCEPTION_THROWN(cache.roadsLayered({{{30.4,113.9,30.8,114.8,true},{}}}),std::invalid_argument);
    }
    void panZoomKeepsVisibleMapBeforeWorkerReturns(){
        ui::Theme theme;theme.apply(*qApp);app::MainWindow window(theme,nullptr,dir_->filePath("cache"));window.show();window.setCurrentPage("map");
        auto* flow=window.findChild<app::NavigationWorkflow*>();QSignalSpy done(flow,&app::NavigationWorkflow::completed);
        flow->load(imported_,30.593,114.305,1);QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,10000);
        auto* canvas=window.findChild<QWidget*>("mapRenderCanvas");auto* map=window.findChild<QWidget*>("mapPage");
        QTRY_VERIFY_WITH_TIMEOUT(canvas->property("regionRevision").toULongLong()>1&&canvas->property("tilesComplete").toBool(),10000);
        const auto point=mapPixel(canvas,*flow->state().network,{114.306,30.593});
        const auto center=QPointF(canvas->width()/2.,canvas->height()/2.);
        const auto original=canvas->grab().toImage();QSignalSpy published(map,SIGNAL(framePublished()));
        auto roadVisible=[&](const QImage& image,QPointF p){
            for(int y=qRound(p.y())-2;y<=qRound(p.y())+2;++y)for(int x=qRound(p.x())-2;x<=qRound(p.x())+2;++x)
                if(image.rect().contains(x,y)&&image.pixelColor(x,y)!=ui::mapColors(false).land)return true;
            return false;};
        QVERIFY(roadVisible(original,point));
        QVERIFY(QMetaObject::invokeMethod(canvas,"zoomed",Q_ARG(double,1.25)));
        const auto smaller=canvas->grab().toImage();QCOMPARE(published.count(),0);QVERIFY(smaller!=original);
        QVERIFY(roadVisible(smaller,center+(QPointF(point)-center)/1.25));
        QVERIFY(QMetaObject::invokeMethod(canvas,"zoomed",Q_ARG(double,.8)));
        const auto restored=canvas->grab().toImage();QCOMPARE(published.count(),0);QVERIFY(restored!=smaller);QVERIFY(roadVisible(restored,point));
        QVERIFY(QMetaObject::invokeMethod(canvas,"panned",Q_ARG(QPointF,QPointF(.05,0))));
        const auto moved=canvas->grab().toImage();QCOMPARE(published.count(),0);QVERIFY(moved!=restored);
        QVERIFY(roadVisible(moved,QPointF(point)-QPointF(canvas->width()*.05,0)));
    }
    void newestMapRegionWins(){
        const auto initial=data::ImportedDataset(imported_.cachePath).roads({30.59,114.30,30.60,114.31,true});
        ui::MapRegionLoader loader;loader.setDataset(imported_,initial);QSignalSpy changes(&loader,&ui::MapRegionLoader::changed);
        loader.request({31.499,115.499,31.502,115.502,true});
        loader.request({30.59,114.30,30.60,114.31,true});
        QTRY_VERIFY_WITH_TIMEOUT(changes.count()>0,10000);QVERIFY(loader.roads()->wayIndex(10));QVERIFY(!loader.roads()->wayIndex(12));
        QTest::qWait(150);QVERIFY(loader.roads()->wayIndex(10));
        // Replacing a dataset invalidates old work even while its worker exits.
        loader.request({31.499,115.499,31.502,115.502,true});loader.setDataset(imported_,initial);
        QTest::qWait(150);QVERIFY(loader.roads()->wayIndex(10));QVERIFY(!loader.roads()->wayIndex(12));
        const auto empty=data::ImportedDataset(imported_.cachePath).roads({32.,116.,32.01,116.01,true},{},true);
        QCOMPARE(empty->wayCount(),quint32(0));
    }
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
            window.findChild<QAction*>("fitNavigationResult")->trigger();QTest::qWait(400);
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
            window.findChild<QAction*>("fitRoadNetwork")->trigger();loop.exec();disconnect(c);
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
