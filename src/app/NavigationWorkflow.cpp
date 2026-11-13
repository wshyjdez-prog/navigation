#include "NavigationWorkflow.h"
#include "contracts/ImportedDataset.h"
#include "data/osmx/OsmxParser.h"
#include "data/osmx/OsmxStore.h"
#include "algorithms/core/BoostAStar.h"
#include "algorithms/fmm/FmmClient.h"
#include "navigation/GeoProjector.h"
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QDir>
#include <QDataStream>
#include <QProcess>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace p1::app {
namespace {
void check(const std::shared_ptr<std::atomic_bool>& c) {
    if(c && c->load()) throw std::runtime_error("已取消");
}
QString rss() {
#ifdef Q_OS_LINUX
    QFile f("/proc/self/status"); if(f.open(QIODevice::ReadOnly)) {
        const auto lines=f.readAll().split('\n'); QStringList values;
        for(const auto& l:lines) if(l.startsWith("VmRSS:")||l.startsWith("VmHWM:")) values.append(QString::fromLatin1(l).simplified());
        return values.join(" / "); }
#endif
    return QStringLiteral("进程内存：当前平台未测");
}
quint32 snap(const RoadNetwork& n,QPointF geo,bool origin) {
    if(!std::isfinite(geo.x())||!std::isfinite(geo.y())||std::abs(geo.x())>180||std::abs(geo.y())>90)
        throw std::runtime_error("起终点必须是有效 WGS84 经纬度");
    const auto index=n.roads->nearestNode(geo.y(),geo.x(),500);
    if(!index) throw std::runtime_error("选点 500 m 内没有路网节点，请靠近道路重选");
    bool routable=false;
    for(auto way:n.roads->incidentWays(*index)) if(n.roads->wayAt(way).attributes.isRoutable()) routable=true;
    if(!routable || (origin&&n.roads->outgoingEdgeCount(*index)==0)) throw std::runtime_error("选点不能作为当前方向的路网端点，请重选");
    return *index;
}
// Driving snap: nearest node (500 m) that a car can leave (origin) or reach.
quint32 snapDriving(const RoadNetwork& n,const algorithms::model::GraphData& g,QPointF geo,bool origin) {
    if(!std::isfinite(geo.x())||!std::isfinite(geo.y())||std::abs(geo.x())>180||std::abs(geo.y())>90)
        throw std::runtime_error("起终点必须是有效 WGS84 经纬度");
    std::vector<char> usable(n.roads->nodeCount(),0);
    for(const auto& e:g.edges) usable[size_t((origin?e.from:e.to).value-1)]=1;
    double best=500;std::optional<quint32> found;
    for(quint32 i=0;i<n.roads->nodeCount();++i){if(!usable[i])continue;const auto& node=n.roads->nodeAt(i);
        const double d=data::osmxDistanceMeters(geo.y(),geo.x(),node.lat,node.lon);if(d<=best){best=d;found=i;}}
    if(!found) throw std::runtime_error("行车模式：选点 500 m 内没有机动车可通行道路，请靠近车行道重选");
    return *found;
}
QString backend(const QString& name) {
    const auto env=qEnvironmentVariable(name=="fmm" ? "P1_FMM_EXECUTABLE":"P1_UBODT_EXECUTABLE");
    if(!env.isEmpty()) return env;
    const auto bundled=QDir(QCoreApplication::applicationDirPath()).filePath("../libexec/"+name);
    return QFileInfo::exists(bundled)?QFileInfo(bundled).absoluteFilePath():QStandardPaths::findExecutable(name);
}
// Minimal ESRI PolyLine writer for the selected FMM loader. Three numeric ID
// fields; two projected metre coordinates per directed edge. No GDAL dependency
// leaks into the application. Files are ephemeral, owned by the matching task.
void writeShape(const RoadNetwork& n,const QString& stem,const std::shared_ptr<std::atomic_bool>& c) {
    const auto count=int(n.graph.edges.size());
    auto header=[&](QDataStream& s,int words) {
        s.setByteOrder(QDataStream::BigEndian); s<<qint32(9994);
        for(int i=0;i<5;++i) { s<<qint32(0); }
        s<<qint32(words);
        s.setByteOrder(QDataStream::LittleEndian); s<<qint32(1000)<<qint32(3);
        double xmin=1e100,ymin=1e100,xmax=-1e100,ymax=-1e100;
        for(const auto& v:n.graph.nodes) { xmin=std::min(xmin,v.position.x_m); xmax=std::max(xmax,v.position.x_m);
            ymin=std::min(ymin,v.position.y_m); ymax=std::max(ymax,v.position.y_m); }
        s<<xmin<<ymin<<xmax<<ymax<<0.0<<0.0<<0.0<<0.0;
    };
    QFile shp(stem+".shp"),shx(stem+".shx"),dbf(stem+".dbf");
    if(!shp.open(QIODevice::WriteOnly)||!shx.open(QIODevice::WriteOnly)||!dbf.open(QIODevice::WriteOnly))
        throw std::runtime_error("无法创建临时 FMM 路网");
    QDataStream s(&shp),x(&shx); s.setFloatingPointPrecision(QDataStream::DoublePrecision);
    x.setFloatingPointPrecision(QDataStream::DoublePrecision); header(s,50+44*count); header(x,50+4*count);
    QByteArray h(129,'\0'); h[0]=3; h[1]=126; h[2]=11; h[3]=3;
    for(int i=0;i<4;++i) { h[4+i]=char(quint32(count)>>(8*i)); }
    h[8]=char(129); h[10]=37;
    const char* names[]={"id","source","target"};
    for(int j=0;j<3;++j) { const auto name=QByteArray(names[j]);
        std::copy(name.begin(),name.end(),h.begin()+32+j*32); h[32+j*32+11]='N'; h[32+j*32+16]=12; }
    h[128]=13; dbf.write(h);
    for(int i=0;i<count;++i) {
        if(i%1024==0) check(c);
        const auto& e=n.graph.edges[size_t(i)]; const auto& a=e.geometry.front(); const auto& b=e.geometry.back();
        s.setByteOrder(QDataStream::BigEndian); s<<qint32(i+1)<<qint32(40);
        s.setByteOrder(QDataStream::LittleEndian); s<<qint32(3)<<std::min(a.x_m,b.x_m)<<std::min(a.y_m,b.y_m)
            <<std::max(a.x_m,b.x_m)<<std::max(a.y_m,b.y_m)<<qint32(1)<<qint32(2)<<qint32(0)<<a.x_m<<a.y_m<<b.x_m<<b.y_m;
        x.setByteOrder(QDataStream::BigEndian); x<<qint32(50+44*i)<<qint32(40);
        dbf.write(" "); for(auto id:{e.id.value,e.from.value,e.to.value}) dbf.write(QByteArray::number(qulonglong(id)).rightJustified(12,' '));
    }
    dbf.write(QByteArray(1,char(26)));
    if(!shp.flush()||!shx.flush()||!dbf.flush() || s.status()!=QDataStream::Ok || x.status()!=QDataStream::Ok)
        throw std::runtime_error("写入临时路网失败");
}
void generateUbodt(const QString& executable,const QString& stem,const QString& output,
                   const std::shared_ptr<std::atomic_bool>& c) {
    if(executable.isEmpty()) throw std::runtime_error("找不到真实 ubodt_gen 后端，请检查安装");
    QProcess p; p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(executable,{"--network",stem+".shp","--network_id","id","--source","source","--target","target",
        "--output",output,"--delta","2000","--log_level","4"});
    if(!p.waitForStarted(5000)) throw std::runtime_error(p.errorString().toStdString());
    QElapsedTimer timer; timer.start(); QByteArray log;
    while(!p.waitForFinished(50)) {
        log+=p.readAll(); if(log.size()>16384) log=log.right(16384);
        if((c&&c->load())||timer.elapsed()>180000) { p.kill(); p.waitForFinished(3000);
            throw std::runtime_error(c&&c->load()?"已取消":"UBODT 预处理超时，请缩小工作区域"); }
    }
    if(p.exitStatus()!=QProcess::NormalExit||p.exitCode()!=0||!QFileInfo::exists(output)||QFileInfo(output).size()==0)
        throw std::runtime_error(("UBODT 生成失败："+QString::fromUtf8(log+p.readAll()).right(800)).toStdString());
}
}
QPointF RoadNetwork::project(QPointF g) const { return {(g.x()-lon0)*metresLon,(g.y()-lat0)*metresLat}; }
QPointF RoadNetwork::unproject(QPointF m) const { return {lon0+m.x()/metresLon,lat0+m.y()/metresLat}; }
std::shared_ptr<const RoadNetwork> NavigationWorkflow::buildNetwork(std::shared_ptr<const data::OsmxDataset> roads,
    const std::shared_ptr<std::atomic_bool>& cancel, quint32 maxNodes, int maxEdges) {
    if(!roads || roads->edges().isEmpty() || !roads->bounds().valid) throw std::runtime_error("数据集没有可规划的有向边");
    if(roads->nodeCount()>maxNodes||roads->edges().size()>maxEdges) throw std::runtime_error("路网过大，请使用区域加载");
    auto n=std::make_shared<RoadNetwork>(); n->roads=std::move(roads);
    n->lat0=n->roads->bounds().centerLat(); n->lon0=n->roads->bounds().centerLon();
    n->metresLat=data::kOsmxEarthRadiusMeters*std::acos(-1.0)/180;
    n->metresLon=n->metresLat*std::cos(n->lat0*std::acos(-1.0)/180);
    if(n->metresLon<1000) throw std::runtime_error("当前局部投影不支持极地区域");
    QCryptographicHash subset(QCryptographicHash::Sha256);
    subset.addData(n->roads->info().sourceSha256.toUtf8());
    for(quint32 wi=0;wi<n->roads->wayCount();++wi) {
        subset.addData(QByteArray::number(n->roads->wayAt(wi).id)+',');
    }
    n->version=QString::fromLatin1(subset.result().toHex());
    n->graph.metadata={n->roads->info().datasetId.toStdString(),n->version.toStdString(),"OSM road snapshot",
        {algorithms::model::CoordinateSystem::LocalCartesian,algorithms::model::LengthUnit::Metres,
         QString("equirectangular WGS84 origin=%1,%2; scales=%3,%4 m/deg").arg(n->lat0,0,'g',17)
            .arg(n->lon0,0,'g',17).arg(n->metresLat,0,'g',17).arg(n->metresLon,0,'g',17).toStdString()}};
    navigation::GeoProjector projector(n->lat0,n->lon0,n->metresLat,n->metresLon);
    n->graph.nodes.reserve(n->roads->nodeCount());
    n->graph.edges.reserve(size_t(n->roads->edges().size()));
    for(quint32 i=0;i<n->roads->nodeCount();++i) {
        if(i%1024==0) { check(cancel); }
        const auto& node=n->roads->nodeAt(i);
        contracts::GeoCoordinate geo; geo.crs=contracts::CoordinateSystem::WGS84;
        geo.latitudeDeg=node.lat; geo.longitudeDeg=node.lon;
        const auto point=projector(geo);
        if(!point || std::abs(node.lat)>90 || std::abs(node.lon)>180) throw std::runtime_error("路网包含非法 WGS84 坐标");
        n->graph.nodes.push_back({algorithms::model::NodeId(i+1),*point});
    }
    for(int i=0;i<n->roads->edges().size();++i) {
        if(i%1024==0) { check(cancel); }
        const auto& e=n->roads->edgeAt(quint32(i));
        if(e.from==e.to||e.lengthMeters<=0) continue;
        n->graph.edges.push_back({algorithms::model::EdgeId(quint64(i)+1),algorithms::model::NodeId(e.from+1),
            algorithms::model::NodeId(e.to+1),{n->graph.nodes[e.from].position,n->graph.nodes[e.to].position},
            e.lengthMeters,e.lengthMeters});
    }
    if(n->graph.edges.empty()) throw std::runtime_error("路网没有有效的非零长度边");
    return n;
}
algorithms::model::GraphData NavigationWorkflow::drivingGraph(const RoadNetwork& n) {
    algorithms::model::GraphData g;g.metadata=n.graph.metadata;g.nodes=n.graph.nodes;
    std::vector<signed char> allowed(n.roads->wayCount(),-1);
    for(const auto& e:n.graph.edges) {
        const auto way=n.roads->edgeAt(quint32(e.id.value-1)).way;
        if(allowed[way]<0) allowed[way]=carAllowed(*n.roads,way)?1:0;
        if(allowed[way]) g.edges.push_back(e);
    }
    return g;
}
void NavigationWorkflow::setRouteMode(RouteMode mode) {
    if(mode==routeMode_) return;
    routeMode_=mode;status_=QString("已切换为%1").arg(routeModeName(mode));emit changed();
}
NavigationWorkflow::NavigationWorkflow(QObject* p):QObject(p) { pool_.setMaxThreadCount(2); }
QString NavigationWorkflow::runtimeStatus() const {
    return QString("工作线程 %1/%2 · %3").arg(pool_.activeThreadCount()).arg(pool_.maxThreadCount()).arg(rss());
}
NavigationWorkflow::~NavigationWorkflow() { for(const auto& c:cancellations_) c->store(true); pool_.waitForDone(); }
void NavigationWorkflow::beginMapPick(bool start) {
    if(busy_) cancel();
    pickMode_=start?1:2;status_=start?"请在地图上单击起点":"请在地图上单击终点";
    emit changed();emit mapPickRequested();
}
void NavigationWorkflow::acceptMapPoint(QPointF geo) {
    if(!pickMode_ || !std::isfinite(geo.x()) || !std::isfinite(geo.y()) ||
        std::abs(geo.x())>180 || std::abs(geo.y())>90) return;
    const bool start=pickMode_==1;pickMode_=0;setEndpoint(start,geo);
}
void NavigationWorkflow::setEndpoint(bool start,QPointF geo) {
    if(!std::isfinite(geo.x())||!std::isfinite(geo.y())||std::abs(geo.x())>180||std::abs(geo.y())>90) return;
    if(busy_) cancel();
    pickMode_=0;
    (start?startPoint_:endPoint_)=geo;
    state_.route.clear();state_.distanceM=0;
    emit endpointSelected(start,geo);
    status_=startPoint_&&endPoint_?"起终点已选好，可计算路线":"请右键地图设置另一个端点";
    emit changed();
}
void NavigationWorkflow::clearEndpoints() {
    if(busy_) cancel();
    pickMode_=0;startPoint_.reset();endPoint_.reset();state_.route.clear();state_.distanceM=0;
    status_="已清除路线；右键地图设置起点和终点";emit changed();
}
void NavigationWorkflow::cancel() { for(const auto& c:cancellations_) c->store(true); ++generation_;
    busy_=false; status_="已取消；先前已发布结果保留"; emit changed(); }
void NavigationWorkflow::run(const QString& kind,std::function<WorkflowResult(std::shared_ptr<std::atomic_bool>)> fn) {
    for(const auto& c:cancellations_) c->store(true);
    const auto generation=++generation_; auto token=std::make_shared<std::atomic_bool>(false); cancellations_.append(token);
    busy_=true; status_=kind+"进行中…可取消"; emit changed();
    auto* w=new QFutureWatcher<WorkflowResult>(this);
    connect(w,&QFutureWatcher<WorkflowResult>::finished,this,[this,w,kind,generation,token] {
        const auto r=w->result(); w->deleteLater(); cancellations_.removeAll(token);
        if(generation!=generation_ || token->load()) return;
        lastResult_=r;
        busy_=false; status_=r.message;
        metrics_=QString("总耗时 %1 ms · 算法/子进程 %2 ms · %3 项\n%4（整个应用进程，非算法独占）")
            .arg(r.elapsedMs,0,'f',1).arg(r.algorithmMs,0,'f',1).arg(r.count).arg(rss());
        if(!r.ok && state_.network) status_ += "（保留上次成功结果）";
        if(r.ok) { if(kind=="加载路网") { state_=r; pickMode_=0; data::OsmxStore::shared().publish(r.network->roads);
                activeDataset_=pendingDataset_;++datasetRevision_;startPoint_.reset();endPoint_.reset();
            }
            else if(kind=="A* 规划") { state_.route=r.route; state_.distanceM=r.distanceM;
                if(r.network) state_.network=r.network;
                if(r.loadedArea.valid) state_.loadedArea=r.loadedArea;
            }
            else { state_.observations=r.observations; state_.matched=r.matched; } }
        emit changed(); emit completed(kind,r.ok);
    });
    w->setFuture(QtConcurrent::run(&pool_,[fn=std::move(fn),token] {
        QElapsedTimer t; t.start(); WorkflowResult r;
        try { check(token); r=fn(token); } catch(const std::exception& e) { r.message=QString::fromUtf8(e.what()); }
        r.elapsedMs=t.nsecsElapsed()/1e6; return r;
    }));
}
void NavigationWorkflow::load(const ImportResult& entry,double lat,double lon,double radiusKm) {
    pendingDataset_=entry;
    run("加载路网",[entry,lat,lon,radiusKm](auto c) {
        WorkflowResult r; std::shared_ptr<const data::OsmxDataset> ds;
        if(entry.sourceFormat=="osmx") {
            const auto parsed=data::OsmxParser().parse(QDir(entry.cachePath).filePath("data.osmx"),{},
                {{},{},[c]{return c->load();}});
            if(!parsed.ok()) { throw std::runtime_error(parsed.error.toString().toStdString()); }
            ds=parsed.dataset;
        } else {
            if(!std::isfinite(lat)||!std::isfinite(lon)||std::abs(lat)>85||std::abs(lon)>180||
                !std::isfinite(radiusKm)||radiusKm<0.2||radiusKm>10) throw std::runtime_error("区域中心或半径无效");
            const double dy=radiusKm*1000/111195.08,dx=dy/std::cos(lat*std::acos(-1.0)/180);
            r.loadedArea={std::max(-85.,lat-dy),std::max(-180.,lon-dx),std::min(85.,lat+dy),std::min(180.,lon+dx),true};
            ds=data::ImportedDataset(entry.cachePath).roads(r.loadedArea,c);
        }
        r.network=buildNetwork(ds,c); r.ok=true; r.count=int(ds->edges().size());
        r.message=QString("真实路网：%1 · %2 节点 / %3 道路 / %4 有向边；规划限当前工作区域")
            .arg(ds->info().sourceFileName).arg(ds->nodeCount()).arg(ds->wayCount()).arg(ds->edges().size()); return r;
    });
}
void NavigationWorkflow::plan(QPointF start,QPointF end) {
    auto n=state_.network;
    startPoint_=start;endPoint_=end;
    const auto entry=activeDataset_;
    const auto loadedArea=state_.loadedArea;
    const auto mode=routeMode_;
    run("A* 规划",[n,entry,start,end,loadedArea,mode](auto c) mutable {
        if(!n) throw std::runtime_error("请先加载路网");
        if(!std::isfinite(start.x())||!std::isfinite(start.y())||!std::isfinite(end.x())||!std::isfinite(end.y())||
            std::abs(start.x())>180||std::abs(end.x())>180||std::abs(start.y())>85||std::abs(end.y())>85)
            throw std::runtime_error("起终点必须是有效 WGS84 经纬度（非极地）");
        auto area=loadedArea;
        const auto& bounds=area.valid?area:n->roads->bounds();
        auto contains=[&](QPointF p){return p.x()>=bounds.minLon&&p.x()<=bounds.maxLon&&p.y()>=bounds.minLat&&p.y()<=bounds.maxLat;};
        const bool regional=entry&&entry->sourceFormat!="osmx";
        // Endpoint rectangle plus `margin` metres on every side.
        auto around=[&](double margin)->data::OsmxBounds{
            const double dy=margin/111195.08,dx=dy/std::cos((start.y()+end.y())*.5*std::acos(-1.)/180.);
            return {std::max(-85.,std::min(start.y(),end.y())-dy),std::max(-180.,std::min(start.x(),end.x())-dx),
                    std::min(85.,std::max(start.y(),end.y())+dy),std::min(180.,std::max(start.x(),end.x())+dx),true};};
        // Long routes exceed the full-detail limits (0.3°x0.5° / 250k nodes);
        // they fall back to the tiered network below instead of failing here.
        QString detailedLimit;
        if(regional&&(!contains(start)||!contains(end))) {
            // Reuse the approved regional adapter; map panning never cancels a
            // route calculation. Query both endpoints with room for nearby links.
            area=around(1000);
            try { n=buildNetwork(data::ImportedDataset(entry->cachePath).roads(area,c),c); }
            catch(const std::exception& e) { if(c&&c->load()) throw; detailedLimit=QString::fromUtf8(e.what()); n.reset(); }
        }
        const bool driving=mode==RouteMode::Driving;
        auto attempt=[&](const RoadNetwork& net,quint32& a,quint32& b){
            const auto car=driving?drivingGraph(net):algorithms::model::GraphData{};
            if(driving&&car.edges.empty()) throw std::runtime_error("行车模式：当前工作区域没有机动车可通行道路");
            const auto& graph=driving?car:net.graph;
            a=driving?snapDriving(net,car,start,true):snap(net,start,true);
            b=driving?snapDriving(net,car,end,false):snap(net,end,false);
            return algorithms::findRoute(graph,algorithms::model::NodeId(a+1),algorithms::model::NodeId(b+1),c);};
        quint32 a=0,b=0;algorithms::RoutePath path;
        if(n) path=attempt(*n,a,b); else path.status=algorithms::RouteStatus::Unreachable;
        // A route may have to leave the loaded rectangle (round a lake, to a
        // bridge, through a one-way system). When no directed path exists, the
        // standard-OSM area grows 2 -> 4 -> 8 km beyond the endpoints and A*
        // runs again, until the network size limit of buildNetwork is reached.
        double expandedTo=0;QString stopped;
        for(double margin:{2000.,4000.,8000.}) {
            if(!regional||!n||path.status!=algorithms::RouteStatus::Unreachable) break;
            check(c);const auto wider=around(margin);
            std::shared_ptr<const RoadNetwork> bigger;
            try { bigger=buildNetwork(data::ImportedDataset(entry->cachePath).roads(wider,c),c); }
            catch(const std::exception& e) { if(c&&c->load()) throw; stopped=QString::fromUtf8(e.what()); break; }
            n=bigger;area=wider;expandedTo=margin;path=attempt(*n,a,b);
        }
        // Tiered fallback (DEC-20261111-1-long-route-01, after Navit's car
        // route_depth "4:25%,8:40000,18:10000"): every road within 3 km of each
        // endpoint, motorway..tertiary within 15 km, and in the endpoint box
        // grown by 25% of the distance (>= 5 km) motorway..primary, or only
        // motorway/trunk beyond 100 km. Same A*, same snapping and route mode.
        bool tiered=false;
        if(regional&&path.status==algorithms::RouteStatus::Unreachable) {
            check(c);
            auto box=[&](QPointF p,double m)->data::OsmxBounds{const double dy=m/111195.08,dx=dy/std::cos(p.y()*std::acos(-1.)/180.);
                return {std::max(-85.,p.y()-dy),std::max(-180.,p.x()-dx),std::min(85.,p.y()+dy),std::min(180.,p.x()+dx),true};};
            const double distance=data::osmxDistanceMeters(start.y(),start.x(),end.y(),end.x());
            const QStringList local{"motorway","motorway_link","trunk","trunk_link","primary","primary_link","secondary","secondary_link","tertiary","tertiary_link"};
            QStringList corridor{"motorway","motorway_link","trunk","trunk_link"};
            if(distance<=100000) corridor<<"primary"<<"primary_link";
            const QVector<data::ImportedDataset::RoadLayer> layers{{box(start,3000),{}},{box(end,3000),{}},
                {box(start,15000),local},{box(end,15000),local},{around(std::max(5000.,distance*.25)),corridor}};
            std::shared_ptr<const RoadNetwork> tieredNet;
            try { tieredNet=buildNetwork(data::ImportedDataset(entry->cachePath).roadsLayered(layers,c),c,1000000,2000000); }
            catch(const std::exception& e) { if(c&&c->load()) throw; stopped=QString("分级路网也无法加载：")+QString::fromUtf8(e.what()); }
            if(tieredNet) {
                quint32 ta=0,tb=0;const auto tpath=attempt(*tieredNet,ta,tb);
                if(tpath.status==algorithms::RouteStatus::Success) {n=tieredNet;area=box(start,3000);path=tpath;tiered=true;}
                else if(path.message.empty()) path=tpath;
            }
        }
        if(path.status!=algorithms::RouteStatus::Success) {
            QString detail=QString::fromStdString(path.message.empty()?std::string("no directed path between the supplied nodes"):path.message);
            if(!detailedLimit.isEmpty()) detail+=QString("（两点间全量路网超出限制：%1；已尝试分级路网）").arg(detailedLimit);
            if(expandedTo>0) detail+=QString("（规划区域已自动扩大到起终点外 %1 km 仍不连通%2）").arg(expandedTo/1000).arg(stopped.isEmpty()?QString():"，"+stopped);
            else if(!stopped.isEmpty()) detail+=QString("（无法继续扩大规划区域：%1）").arg(stopped);
            throw std::runtime_error(("无法规划："+detail).toStdString());
        }
        if(path.geometry.size()<2) throw std::runtime_error("起终点吸附到同一节点，请拉开距离");
        WorkflowResult r; r.ok=true; r.network=n; r.loadedArea=area;r.distanceM=path.total_distance_m; r.algorithmMs=path.elapsed_ms; r.count=int(path.edges.size());
        for(const auto& p:path.geometry) {
            const auto geo=n->unproject({p.x_m,p.y_m});
            if(r.route.isEmpty() || r.route.last()!=geo) r.route.append(geo);
        }
        r.message=QString("%1 · A* 路线 %2 m · %3 条边；两端 500 m 内吸附，遵循单行线/通行限制%4").arg(routeModeName(mode)).arg(r.distanceM,0,'f',1).arg(r.count)
            .arg(driving?"，不含人行道/步道/台阶等非机动车道路（参照 Navit 车辆配置）":"");
        if(tiered) r.message+=QString("；远距离：按 Navit route_depth 方式分级加载（两端 3 km 全部道路、15 km 内三级路以上、走廊内主干道），中段只走主干道，可能不是严格最短");
        else if(expandedTo>0) r.message+=QString("；路线需绕出原区域，规划区域已自动扩大到起终点外 %1 km").arg(expandedTo/1000);
        return r;
    });
}
void NavigationWorkflow::match(const QString& path) {
    auto n=state_.network;
    run("FMM 匹配",[n,path](auto c) {
        if(!n) throw std::runtime_error("请先加载路网");
        QFile f(path); if(!f.open(QIODevice::ReadOnly)||f.size()>1024*1024) throw std::runtime_error("轨迹文件不可读或超过 1 MiB");
        QTextStream in(&f); if(in.readLine().trimmed()!="latitude,longitude")
            throw std::runtime_error("CSV 首行须为 latitude,longitude，坐标为 WGS84 度");
        WorkflowResult r; std::vector<algorithms::FmmPoint> points;
        while(!in.atEnd()) { check(c); const auto line=in.readLine().trimmed(); if(line.isEmpty()) continue;
            const auto a=line.split(','); bool ok1=false,ok2=false;
            const double lat=a.value(0).toDouble(&ok1),lon=a.value(1).toDouble(&ok2);
            if(a.size()!=2||!ok1||!ok2||!std::isfinite(lat)||!std::isfinite(lon)||!n->roads->bounds().contains(lat,lon))
                throw std::runtime_error("轨迹有非法坐标或位于已加载工作区域之外");
            r.observations.append({lon,lat}); const auto p=n->project({lon,lat}); points.push_back({p.x(),p.y()});
            if(points.size()>256) throw std::runtime_error("本次匹配支持 2–256 个轨迹点，请裁剪轨迹");
        }
        if(points.size()<2) throw std::runtime_error("匹配至少需要 2 个轨迹点");
        const QString fmm=backend("fmm"),ubodt=backend("ubodt_gen");
        if(fmm.isEmpty()) throw std::runtime_error("找不到真实 FMM 后端，请检查安装");
        QTemporaryDir temp; if(!temp.isValid()) throw std::runtime_error("临时目录创建失败");
        const auto stem=temp.filePath("network"),table=temp.filePath("ubodt.csv");
        writeShape(*n,stem,c); generateUbodt(ubodt,stem,table,c); check(c);
        algorithms::FmmConfig config; config.executable=fmm; config.networkFile=stem+".shp"; config.ubodtFile=table;
        config.datasetId=n->roads->info().datasetId; config.networkVersion=n->version; config.ubodtNetworkVersion=n->version;
        config.coordinateDefinition=QString::fromStdString(n->graph.metadata.frame.definition); config.timeoutMs=120000;
        for(const auto& e:n->graph.edges) config.p0EdgeIds.emplace(qint64(e.id.value),e.id);
        const auto matched=algorithms::runFmm(config,points,c);
        if(matched.status!=algorithms::FmmStatus::Matched) throw std::runtime_error((matched.message+" "+matched.processLog.right(500)).toStdString());
        for(const auto& p:matched.matchedGeometry) r.matched.append(n->unproject({p.x,p.y}));
        if(r.matched.size()<2) throw std::runtime_error("匹配没有返回可绘制几何");
        r.ok=true; r.count=int(points.size()); r.algorithmMs=matched.elapsedMs;
        r.message=QString("真实 FMM/HMM 匹配成功 · %1 点 / %2 条路径边；UBODT 阈值 2000 m，断点间距须在阈值内")
            .arg(points.size()).arg(matched.completePath.size()); return r;
    });
}
}
