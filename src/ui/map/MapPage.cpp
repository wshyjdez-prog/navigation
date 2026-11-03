#include "MapPage.h"
#include "app/NavigationWorkflow.h"
#include "contracts/ImportedDataset.h"
#include "data/osmx/OsmxCache.h"
#include "pal/linux/renderer/LinuxRenderer.h"
#include "ui/common/Components.h"
#include "ui/common/Theme.h"
#include <QtConcurrent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QWheelEvent>
#include <QElapsedTimer>
#include <QSaveFile>
#include <cmath>

namespace p1::ui {
namespace {
class Controls final : public QWidget {
    Q_OBJECT
public:
    Controls(Theme&,app::NavigationWorkflow* flow,QString directory):flow_(flow),directory_(std::move(directory)) {
        auto* l=new QVBoxLayout(this);l->setContentsMargins(0,0,0,0);l->setSpacing(12);
        auto* tabs=new QTabWidget;l->addWidget(tabs);
        auto* dataPage=new QWidget;auto* dl=new QVBoxLayout(dataPage);
        dl->addWidget(label("工作区域","sectionTitle"));
        datasets_=new QComboBox;datasets_->setObjectName("mapDatasetSelector");datasets_->setMinimumContentsLength(8);
        datasets_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        dl->addWidget(label("已导入的数据集","caption"));dl->addWidget(datasets_);
        auto* refresh=button("刷新数据集");refresh->setObjectName("refreshMapDatasets");dl->addWidget(refresh);
        auto* form=new QFormLayout;lat_=coordinate("regionLatitude",-85,85,30.593);lon_=coordinate("regionLongitude",-180,180,114.305);
        radius_=coordinate("regionRadiusKm",0.2,10,1);radius_->setDecimals(1);radius_->setSuffix(" km");radius_->setSingleStep(0.2);
        form->addRow("中心纬度",lat_);form->addRow("中心经度",lon_);form->addRow("工作半径",radius_);dl->addLayout(form);
        auto* load=button("加载真实地图",true);load->setObjectName("loadRoadNetwork");dl->addWidget(load);
        auto* note=label("标准 OSM 按区域加载完整道路；OSMX 加载整个已裁剪路网。规划与匹配限已加载范围。","caption");note->setWordWrap(true);dl->addWidget(note);dl->addStretch();tabs->addTab(dataPage,"数据");
        auto* routePage=new QWidget;auto* rl=new QVBoxLayout(routePage);auto* rf=new QFormLayout;
        startLat_=coordinate("routeStartLatitude",-90,90,30.593);startLon_=coordinate("routeStartLongitude",-180,180,114.305);
        endLat_=coordinate("routeEndLatitude",-90,90,30.597);endLon_=coordinate("routeEndLongitude",-180,180,114.309);
        rf->addRow("起点纬度",startLat_);rf->addRow("起点经度",startLon_);rf->addRow("终点纬度",endLat_);rf->addRow("终点经度",endLon_);rl->addLayout(rf);
        auto* pickStart=button("地图选起点");auto* pickEnd=button("地图选终点");pickStart->setObjectName("pickRouteStart");pickEnd->setObjectName("pickRouteEnd");rl->addWidget(pickStart);rl->addWidget(pickEnd);
        route_=button("计算 A* 路线",true);route_->setObjectName("calculateRoute");rl->addWidget(route_);
        auto* routeNote=label("WGS84 度；500 m 内吸附节点。通用路网距离最短，可包含步行/骑行道路；非汽车专用规则，不含转向限制与实时路况。","caption");routeNote->setWordWrap(true);rl->addWidget(routeNote);rl->addStretch();tabs->addTab(routePage,"路线");
        auto* matchPage=new QWidget;auto* ml=new QVBoxLayout(matchPage);
        auto* format=label("CSV 首行：latitude,longitude\n后续每行一对 WGS84 纬度、经度。支持 2–256 点；相邻点沿路距离不超过 2000 m。","secondary");format->setWordWrap(true);ml->addWidget(format);
        match_=button("选择轨迹并运行 FMM",true);match_->setObjectName("matchTrajectory");ml->addWidget(match_);
        auto* exportRoute=button("导出路线为测试轨迹");exportRoute->setObjectName("exportRouteTrajectory");ml->addWidget(exportRoute);
        auto* legend=label("灰线：原始轨迹\n橙线：FMM/HMM 匹配路径\n紫线：A* 规划路线\n\n后台生成本区域 UBODT；取消会终止子进程，任务结束清理临时文件。","caption");legend->setWordWrap(true);ml->addWidget(legend);ml->addStretch();tabs->addTab(matchPage,"轨迹");
        auto* cancel=button("取消当前任务");cancel->setObjectName("cancelNavigationTask");l->addWidget(cancel);
        status_=label(flow_->status(),"secondary");status_->setObjectName("navigationStatus");status_->setWordWrap(true);l->addWidget(status_);
        metrics_=label({},"caption");metrics_->setObjectName("algorithmMetrics");metrics_->setWordWrap(true);l->addWidget(metrics_);
        auto* live=label({},"caption");live->setObjectName("workerStatus");live->setWordWrap(true);l->addWidget(live);
        auto* heartbeat=new QTimer(this);heartbeat->setInterval(100);auto clock=std::make_shared<QElapsedTimer>();clock->start();
        connect(heartbeat,&QTimer::timeout,this,[this,live,clock]{const auto gap=clock->restart();
            live->setText(QString("UI 心跳间隔 %1 ms · %2\n进程 RSS/HWM 含界面与渲染，非算法独占内存").arg(gap).arg(flow_->runtimeStatus()));});heartbeat->start();
        connect(refresh,&QPushButton::clicked,this,&Controls::refresh);
        connect(load,&QPushButton::clicked,this,[this]{int i=datasets_->currentIndex();if(i>=0&&i<entries_.size())flow_->load(entries_[i],lat_->value(),lon_->value(),radius_->value());});
        connect(route_,&QPushButton::clicked,this,[this]{flow_->plan({startLon_->value(),startLat_->value()},{endLon_->value(),endLat_->value()});});
        connect(match_,&QPushButton::clicked,this,[this]{const auto p=QFileDialog::getOpenFileName(this,"选择 WGS84 轨迹 CSV",{},"CSV (*.csv)");if(!p.isEmpty())flow_->match(p);});
        connect(exportRoute,&QPushButton::clicked,this,[this]{const auto& route=flow_->state().route;
            if(route.size()<2){status_->setText("请先计算路线，再导出沿路线生成的测试轨迹（非实采 GPS）");return;}
            const auto path=QFileDialog::getSaveFileName(this,"导出合成测试轨迹（非实采 GPS）","route-synthetic.csv","CSV (*.csv)");if(path.isEmpty())return;
            QByteArray csv("latitude,longitude\n");const int count=std::min(256,int(route.size()));
            for(int i=0;i<count;++i){const auto& p=route.at(qsizetype(i)*(route.size()-1)/(count-1));
                csv+=QString("%1,%2\n").arg(p.y(),0,'f',9).arg(p.x(),0,'f',9).toUtf8();}
            QSaveFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(csv)!=csv.size()||!file.commit())status_->setText("轨迹导出失败");
            else status_->setText(QString("已导出 %1 点合成测试轨迹；可选择此 CSV 检查 FMM。非实采 GPS。").arg(count));});
        connect(cancel,&QPushButton::clicked,flow_,&app::NavigationWorkflow::cancel);
        connect(pickStart,&QPushButton::clicked,this,[this]{flow_->beginMapPick(true);});
        connect(pickEnd,&QPushButton::clicked,this,[this]{flow_->beginMapPick(false);});
        connect(flow_,&app::NavigationWorkflow::endpointSelected,this,[this](bool start,QPointF p){
            (start?startLat_:endLat_)->setValue(p.y());(start?startLon_:endLon_)->setValue(p.x());});
        connect(flow_,&app::NavigationWorkflow::changed,this,[this,cancel,load]{status_->setText(flow_->status());metrics_->setText(flow_->metrics());
            route_->setEnabled(!flow_->busy()&&bool(flow_->state().network));match_->setEnabled(route_->isEnabled());cancel->setEnabled(flow_->busy());load->setEnabled(!flow_->busy()&&!entries_.isEmpty());});
        cancel->setEnabled(false);route_->setEnabled(false);match_->setEnabled(false);load->setEnabled(false);
        connect(this,&Controls::catalogReady,load,[load](bool ready){load->setEnabled(ready);});this->refresh();
    }
    ~Controls() override {if(catalog_)catalog_->waitForFinished();}
signals: void catalogReady(bool ready);
private:
    QDoubleSpinBox* coordinate(const char* name,double min,double max,double value){auto* b=new QDoubleSpinBox;b->setObjectName(name);b->setDecimals(7);b->setRange(min,max);b->setValue(value);b->setSingleStep(0.001);b->setButtonSymbols(QAbstractSpinBox::NoButtons);return b;}
    void refresh(){if(catalog_&&catalog_->isRunning())return;auto* w=new QFutureWatcher<QVector<app::ImportResult>>(this);catalog_=w;
        connect(w,&QFutureWatcher<QVector<app::ImportResult>>::finished,this,[this,w]{const auto previous=datasets_->currentData().toString();entries_=w->result();datasets_->clear();
            for(const auto& e:entries_)datasets_->addItem(e.fileName+" · "+e.sourceFormat,e.datasetId);
            const int i=datasets_->findData(previous);if(i>=0)datasets_->setCurrentIndex(i);
            if(entries_.isEmpty()) { status_->setText("请先在数据导入页导入文件"); }
            emit catalogReady(!entries_.isEmpty()&&!flow_->busy());catalog_=nullptr;w->deleteLater();});
        const auto dir=directory_;w->setFuture(QtConcurrent::run([dir]{QVector<app::ImportResult> all;const auto root=dir.isEmpty()?data::defaultImportCacheDirectory():dir;
            for(const auto& f:QDir(root).entryInfoList({"*.sqlite"},QDir::Files,QDir::Time))try{all.append(data::ImportedDataset(f.absoluteFilePath()).summary());}catch(const std::exception&){}
            all+=data::OsmxCache::list(dir.isEmpty()?data::OsmxCache::defaultDirectory():dir).entries;return all;}));}
    app::NavigationWorkflow* flow_;QString directory_;QComboBox* datasets_;QVector<app::ImportResult> entries_;
    QDoubleSpinBox *lat_,*lon_,*radius_,*startLat_,*startLon_,*endLat_,*endLon_;QLabel *status_,*metrics_;QPushButton *route_,*match_;
    QFutureWatcher<QVector<app::ImportResult>>* catalog_=nullptr;
};
QRectF aspectView(QRectF v,QSize size){const double aspect=double(std::max(1,size.width()))/std::max(1,size.height());
    if(v.width()/v.height()<aspect){double d=(v.height()*aspect-v.width())/2;v.adjust(-d,0,d,0);}else{double d=(v.width()/aspect-v.height())/2;v.adjust(0,-d,0,d);}return v;}
QPointF screenPoint(QPointF p,QRectF v,QSize s){return{(p.x()-v.left())/v.width()*s.width(),(v.bottom()-p.y())/v.height()*s.height()};}
class MapCanvas final : public QWidget {
    Q_OBJECT
public:
    MapCanvas(){setObjectName("mapRenderCanvas");setMinimumSize(300,260);setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);}
    void setFrame(const QImage& f){frame_=f;setProperty("hasFrame",true);update();}
signals: void resized();void zoomed(double);void panned(QPointF);void clicked(QPointF);
protected:
    void paintEvent(QPaintEvent*) override{QPainter p(this);p.fillRect(rect(),palette().window());if(frame_.isNull()){p.setPen(palette().text().color());p.drawText(rect(),Qt::AlignCenter,"加载真实路网后显示地图");}else p.drawImage(rect(),frame_);}
    void resizeEvent(QResizeEvent*) override{emit resized();}
    void wheelEvent(QWheelEvent* e) override{emit zoomed(e->angleDelta().y()>0?0.8:1.25);e->accept();}
    void mousePressEvent(QMouseEvent* e) override{if(e->button()==Qt::LeftButton){pressed_=e->position();last_=pressed_;drag_=true;}}
    void mouseMoveEvent(QMouseEvent* e) override{if(drag_){const auto d=e->position()-last_;last_=e->position();if((e->position()-pressed_).manhattanLength()>4)emit panned({-d.x()/width(),d.y()/height()});}}
    void mouseReleaseEvent(QMouseEvent* e) override{if(drag_&&(e->position()-pressed_).manhattanLength()<=4)emit clicked({e->position().x()/width(),e->position().y()/height()});drag_=false;}
private:QImage frame_;QPointF pressed_,last_;bool drag_=false;
};
struct Prepared{renderer::RenderResult render;double totalMs=0;QImage tile;QString tileKey;};
Prepared draw(const app::WorkflowResult& state,QRectF view,QSize size,Colors colors,QImage cache,QString key,QImage previousTile,QString previousTileKey){
    QElapsedTimer timer;timer.start();Prepared out;renderer::MapScene scene;scene.logicalSize=size;scene.source=renderer::MapSource::Real;scene.dataVersion=state.network?state.network->version:"empty";
    auto screen=[&](QPointF geo){return screenPoint(state.network->project(geo),view,size);};
    const auto tileKey=scene.dataVersion+QString("/%1/%2/%3/%4/%5").arg(view.left(),0,'g',17).arg(view.top(),0,'g',17).arg(view.width(),0,'g',17).arg(view.height(),0,'g',17).arg(colors.canvas);
    QImage base;
    if(previousTileKey==tileKey && previousTile.size()==size) { base=previousTile; }
    else {
    base=QImage(size,QImage::Format_ARGB32_Premultiplied);base.fill(QColor(colors.surface));QPainter p(&base);p.setRenderHint(QPainter::Antialiasing);
    if(state.network){const auto& ds=*state.network->roads;const auto a=state.network->unproject(view.topLeft()),b=state.network->unproject(view.bottomRight());
        const auto visible=ds.waysInBounds({a.y(),a.x(),b.y(),b.x(),true});int labels=0;
        for(auto wi:visible){const auto& way=ds.wayAt(wi);QPainterPath path;QPointF middle;
            for(quint32 j=0;j<way.refCount;++j){const auto& node=ds.nodeAt(ds.wayNodePool().at(int(way.refFirst+j)));auto pt=screen({node.lon,node.lat});if(j==0)path.moveTo(pt);else path.lineTo(pt);if(j==way.refCount/2)middle=pt;}
            const bool major=way.attributes.roadClass>=data::RoadClass::Motorway&&way.attributes.roadClass<=data::RoadClass::TertiaryLink;
            p.setPen(QPen(QColor(colors.border),major?6:3,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.drawPath(path);
            p.setPen(QPen(QColor(major?colors.selected:colors.raised),major?4:1.5,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.drawPath(path);
            auto name=ds.displayName(wi);if(view.width()<1600&&!name.isEmpty()&&labels<8&&QRectF(QPointF(),size).contains(middle)){p.setPen(QColor(colors.secondary));p.drawText(middle+QPointF(4,-4),name);++labels;}}
        p.setPen(QColor(colors.secondary));p.drawText(QPointF(16,size.height()-16),"© OpenStreetMap contributors · WGS84 · 本地道路地图");
    }else{p.setPen(QColor(colors.secondary));p.drawText(base.rect(),Qt::AlignCenter,"请在左侧加载真实路网\n拖动平移 · 滚轮缩放 · 路线页选点");}
    p.end();
    }
    out.tile=base;out.tileKey=tileKey;
    scene.tiles.append({tileKey,QRectF(QPointF(),size),base,0,tileKey,renderer::MapSource::Real});
    auto line=[&](const QVector<QPointF>& points,const QString& id,QColor color,double width,int z){if(!state.network||points.size()<2)return;
        renderer::RouteLine r;r.id=id;r.source=renderer::MapSource::Real;r.dataVersion=scene.dataVersion;r.color=color;r.width=width;r.zOrder=z;
        for(const auto& pt:points) { r.points.append(screen(pt)); }
        scene.routes.append(r);};
    line(state.observations,"trajectory",QColor(colors.secondary),2,10);line(state.route,"astar",QColor(colors.primary),7,12);line(state.matched,"fmm",QColor("#C16A1A"),5,14);
    if(!state.route.isEmpty()){scene.markers.append({"start",screen(state.route.front()),renderer::MarkerKind::Start,QColor(colors.success),"起",8,20});
        scene.markers.append({"end",screen(state.route.back()),renderer::MarkerKind::End,QColor(colors.error),"终",8,20});}
    out.render=renderer::LinuxRenderer::render(scene,nullptr,size,renderer::RenderMode::CachedBase,0,cache,key);out.totalMs=timer.nsecsElapsed()/1e6;return out;
}
class MapPage final : public QWidget {
    Q_OBJECT
public:
    MapPage(Theme& theme,app::NavigationWorkflow* flow,const QString& cache,QWidget* parent):QWidget(parent),theme_(theme),flow_(flow){
        setObjectName("mapPage");auto* l=new QVBoxLayout(this);l->setContentsMargins(0,0,0,0);l->setSpacing(12);auto* toolbar=new QHBoxLayout;
        toolbar->addWidget(badge("OSM 本地路网","info"));auto* fitButton=button("显示全部");fitButton->setObjectName("fitRoadNetwork");toolbar->addWidget(fitButton);
        auto* resultsButton=button("查看结果");resultsButton->setObjectName("fitNavigationResult");toolbar->addWidget(resultsButton);
        auto* minus=button("−");auto* plus=button("＋");toolbar->addWidget(minus);toolbar->addWidget(plus);stats_=label("等待加载地图","caption");stats_->setObjectName("mapRenderStats");stats_->setWordWrap(true);stats_->setMinimumWidth(0);toolbar->addWidget(stats_,1);l->addLayout(toolbar);
        auto* mapRow=new QHBoxLayout;mapRow->setSpacing(12);controls_=new Controls(theme,flow,cache);auto* scroll=scrollable(controls_);scroll->setFixedWidth(320);mapRow->addWidget(scroll);
        canvas_=new MapCanvas;mapRow->addWidget(canvas_,1);l->addLayout(mapRow,1);
        connect(fitButton,&QPushButton::clicked,this,&MapPage::fit);connect(plus,&QPushButton::clicked,this,[this]{zoom(.8);});connect(minus,&QPushButton::clicked,this,[this]{zoom(1.25);});
        connect(resultsButton,&QPushButton::clicked,this,[this]{if(!network_)return;
            QVector<QPointF> points=flow_->state().route;points+=flow_->state().matched;if(points.isEmpty())return;
            const auto first=network_->project(points.first());double left=first.x(),right=left,top=first.y(),bottom=top;
            for(const auto& geo:points){const auto p=network_->project(geo);left=std::min(left,p.x());right=std::max(right,p.x());top=std::min(top,p.y());bottom=std::max(bottom,p.y());}
            const double padding=std::max(80.,std::max(right-left,bottom-top)*.2);view_=QRectF(QPointF(left-padding,top-padding),QPointF(right+padding,bottom+padding));schedule();});
        connect(canvas_,&MapCanvas::zoomed,this,&MapPage::zoom);
        connect(canvas_,&MapCanvas::panned,this,[this](QPointF d){auto a=aspectView(view_,canvas_->size());view_.translate(d.x()*a.width(),d.y()*a.height());schedule();});
        connect(canvas_,&MapCanvas::clicked,this,[this](QPointF f){if(!network_)return;auto a=aspectView(view_,canvas_->size());flow_->acceptMapPoint(network_->unproject({a.left()+f.x()*a.width(),a.bottom()-f.y()*a.height()}));});
        connect(canvas_,&MapCanvas::resized,this,&MapPage::schedule);connect(flow_,&app::NavigationWorkflow::changed,this,[this]{if(network_!=flow_->state().network){network_=flow_->state().network;fit();}else schedule();});
        connect(&theme_,&Theme::changed,this,&MapPage::schedule);debounce_.setSingleShot(true);debounce_.setInterval(30);connect(&debounce_,&QTimer::timeout,this,&MapPage::render);schedule();
    }
    ~MapPage() override{if(watcher_)watcher_->waitForFinished();}
signals:
    void framePublished();
private:
    void zoom(double f){if(!network_)return;auto c=view_.center();double w=std::clamp(view_.width()*f,50.,100000.),h=w*view_.height()/view_.width();view_=QRectF(c-QPointF(w/2,h/2),QSizeF(w,h));schedule();}
    void fit(){if(!network_)return;auto b=network_->roads->bounds();view_=QRectF(network_->project({b.minLon,b.minLat}),network_->project({b.maxLon,b.maxLat}));
        if(view_.width()<100) { view_.setWidth(100); }
        if(view_.height()<100) { view_.setHeight(100); }
        view_.adjust(-view_.width()*.05,-view_.height()*.05,view_.width()*.05,view_.height()*.05);schedule();}
    void schedule(){++revision_;debounce_.start();}
    void render(){if(watcher_){pending_=true;return;}if(canvas_->width()<2||canvas_->height()<2)return;
        auto rev=revision_;auto state=flow_->state();auto size=canvas_->size().boundedTo(QSize(1920,1200));auto view=aspectView(view_.isValid()?view_:QRectF(-500,-500,1000,1000),size);
        auto colors=theme_.colors();auto image=cache_;auto key=key_;auto tile=tile_;auto tileKey=tileKey_;auto* w=new QFutureWatcher<Prepared>(this);watcher_=w;
        connect(w,&QFutureWatcher<Prepared>::finished,this,[this,w,rev]{auto r=w->result();watcher_=nullptr;w->deleteLater();
            if(rev==revision_){if(r.render.error.isEmpty()){canvas_->setFrame(r.render.frame);cache_=r.render.baseCache;key_=r.render.baseCacheKey;tile_=r.tile;tileKey_=r.tileKey;
                canvas_->setProperty("realDataset",bool(flow_->state().network));canvas_->setProperty("routeCount",r.render.stats.routeCount);
                canvas_->setProperty("cacheHit",r.render.stats.cacheHit);canvas_->setProperty("renderElapsedMs",r.totalMs);
                stats_->setText(QString("绘制 %1 ms · %2 条结果线 · 底图%3").arg(r.totalMs,0,'f',1).arg(r.render.stats.routeCount).arg(r.render.stats.cacheHit?"缓存":"更新"));emit framePublished();}else stats_->setText(r.render.error);}
            if(pending_||rev!=revision_){pending_=false;debounce_.start();}});
        w->setFuture(QtConcurrent::run([state,view,size,colors,image,key,tile,tileKey]{return draw(state,view,size,colors,image,key,tile,tileKey);}));}
    Theme& theme_;app::NavigationWorkflow* flow_;Controls* controls_;MapCanvas* canvas_;QLabel* stats_;std::shared_ptr<const app::RoadNetwork> network_;
    QRectF view_;QTimer debounce_;quint64 revision_=0;bool pending_=false;QFutureWatcher<Prepared>* watcher_=nullptr;QImage cache_,tile_;QString key_,tileKey_;
};
}
QWidget* createMapPage(Theme& theme,QWidget* parent){auto* flow=new app::NavigationWorkflow(parent);auto* page=new MapPage(theme,flow,{},parent);if(!parent)flow->setParent(page);return page;}
QWidget* createMapPage(Theme& theme,app::NavigationWorkflow* flow,const QString& cache,QWidget* parent){return new MapPage(theme,flow,cache,parent);}
QWidget* createAlgorithmsPage(Theme& theme,app::NavigationWorkflow* flow,const QString& cache,QWidget* parent){auto* page=new QWidget(parent);page->setObjectName("algorithmsPage");
    auto* l=new QVBoxLayout(page);l->setContentsMargins(0,0,0,0);l->setSpacing(12);l->addWidget(label("A* / FMM 算法工作台","sectionTitle"));
    auto* note=label("与地图页共享路网及结果。Boost.Graph A*（h=0）与固定版本 FMM/HMM 均在后台运行；完成后到地图页查看路径。","secondary");note->setWordWrap(true);l->addWidget(note);l->addWidget(scrollable(new Controls(theme,flow,cache)),1);return page;}
}
#include "MapPage.moc"
