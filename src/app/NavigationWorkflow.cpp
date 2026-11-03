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
    const std::shared_ptr<std::atomic_bool>& cancel) {
    if(!roads || roads->edges().isEmpty() || !roads->bounds().valid) throw std::runtime_error("数据集没有可规划的有向边");
    if(roads->nodeCount()>250000||roads->edges().size()>500000) throw std::runtime_error("路网过大，请使用区域加载");
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
    emit endpointSelected(pickMode_==1,geo);pickMode_=0;
    status_="已选择坐标，请计算 A* 路线";emit changed();
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
                const auto& roads=*r.network->roads;
                // Prefill endpoints from a real directed way, instead of using
                // arbitrary geographic defaults which may lie on disconnected roads.
                bool preset=false;
                for(int pass=0;pass<2&&!preset;++pass) for(quint32 wi=0;wi<roads.wayCount();++wi) {
                    const auto& way=roads.wayAt(wi);
                    if(!way.attributes.isRoutable()||way.refCount<2||
                        (pass==0&&(way.refCount<3||way.attributes.roadClass!=data::RoadClass::Residential))) continue;
                    const auto ai=roads.wayNodePool().at(int(way.refFirst));
                    const auto bi=roads.wayNodePool().at(int(way.refFirst+way.refCount-1));
                    const auto& a=roads.nodeAt(ai);const auto& b=roads.nodeAt(bi);
                    if(ai==bi||roads.nearestNode(a.lat,a.lon)!=ai||roads.nearestNode(b.lat,b.lon)!=bi) continue;
                    QPointF from(a.lon,a.lat),to(b.lon,b.lat);
                    if(way.attributes.isOnewayReversed()) std::swap(from,to);
                    emit endpointSelected(true,from);emit endpointSelected(false,to);preset=true;break;
                }
            }
            else if(kind=="A* 规划") { state_.route=r.route; state_.distanceM=r.distanceM; }
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
            ds=data::ImportedDataset(entry.cachePath).roads({lat-dy,lon-dx,lat+dy,lon+dx,true},c);
        }
        r.network=buildNetwork(ds,c); r.ok=true; r.count=int(ds->edges().size());
        r.message=QString("真实路网：%1 · %2 节点 / %3 道路 / %4 有向边；规划限当前工作区域")
            .arg(ds->info().sourceFileName).arg(ds->nodeCount()).arg(ds->wayCount()).arg(ds->edges().size()); return r;
    });
}
void NavigationWorkflow::plan(QPointF start,QPointF end) {
    auto n=state_.network;
    run("A* 规划",[n,start,end](auto c) {
        if(!n) throw std::runtime_error("请先加载路网");
        const auto a=snap(*n,start,true),b=snap(*n,end,false);
        const auto path=algorithms::findRoute(n->graph,algorithms::model::NodeId(a+1),algorithms::model::NodeId(b+1),c);
        if(path.status!=algorithms::RouteStatus::Success) throw std::runtime_error(("无法规划："+path.message).c_str());
        if(path.geometry.size()<2) throw std::runtime_error("起终点吸附到同一节点，请拉开距离");
        WorkflowResult r; r.ok=true; r.distanceM=path.total_distance_m; r.algorithmMs=path.elapsed_ms; r.count=int(path.edges.size());
        for(const auto& p:path.geometry) {
            const auto geo=n->unproject({p.x_m,p.y_m});
            if(r.route.isEmpty() || r.route.last()!=geo) r.route.append(geo);
        }
        r.message=QString("A* 路线 %1 m · %2 条边；两端 500 m 内吸附，遵循单行线/通行限制").arg(r.distanceM,0,'f',1).arg(r.count);
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
