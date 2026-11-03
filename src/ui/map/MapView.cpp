#include "MapPage.h"
#include "TilePyramid.h"
#include "app/NavigationWorkflow.h"
#include "contracts/ImportedDataset.h"
#include "data/osmx/OsmxCache.h"
#include "pal/linux/renderer/LinuxRenderer.h"
#include "MapFrame.h"
#include "ui/common/Components.h"
#include "ui/common/Theme.h"
#include <QtConcurrent>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QShortcut>
#include <QTimer>
#include <QWheelEvent>
#include <cmath>

namespace p1::ui {
namespace {
QRectF aspectView(QRectF v,QSize size) {
    const double aspect=double(std::max(1,size.width()))/std::max(1,size.height());
    if(v.width()/v.height()<aspect) {double d=(v.height()*aspect-v.width())/2;v.adjust(-d,0,d,0);}
    else {double d=(v.width()/aspect-v.height())/2;v.adjust(0,-d,0,d);}
    return v;
}
QPointF screenPoint(QPointF p,QRectF v,QSize s) {
    return {(p.x()-v.left())/v.width()*s.width(),(v.bottom()-p.y())/v.height()*s.height()};
}
class MapCanvas final : public QWidget {
    Q_OBJECT
public:
    MapCanvas() {
        setObjectName("mapRenderCanvas");setMinimumSize(300,260);
        setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
        setFocusPolicy(Qt::StrongFocus);setCursor(Qt::OpenHandCursor);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAccessibleName("地图：拖动浏览，滚轮缩放，右键或菜单键设置起终点");
    }
    void setFrame(const QImage& frame,QRectF view) {frame_=frame;frameView_=view;setProperty("hasFrame",true);update();}
    void setViewport(QRectF view,QColor background) {view_=view;background_=background;update();}
    void setNotice(const QString& text) {notice_=text;setProperty("mapNotice",text);update();}
signals:
    void resized();
    void zoomed(double factor);
    void panned(QPointF fraction);
    void clicked(QPointF fraction);
    void menuRequested(QPoint position);
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);p.fillRect(rect(),background_);
        if(!frame_.isNull()) p.drawImage(mapFrameTargetRect(frameView_,view_,size()),frame_);
        else {p.setPen(palette().text().color());p.drawText(rect(),Qt::AlignCenter,"从菜单选择已导入地图，或先导入地图文件");}
        if(!notice_.isEmpty()) {
            const QRect bounds(20,height()-118,std::max(1,width()-40),48);
            const auto textRect=p.fontMetrics().boundingRect(bounds,Qt::AlignLeft|Qt::AlignVCenter|Qt::TextWordWrap,notice_);
            p.fillRect(textRect.adjusted(-8,-4,8,4),palette().base());
            p.setPen(palette().text().color());p.drawText(bounds,Qt::AlignLeft|Qt::AlignVCenter|Qt::TextWordWrap,notice_);
        }
    }
    void resizeEvent(QResizeEvent*) override {emit resized();}
    void wheelEvent(QWheelEvent* e) override {
        const int delta=e->angleDelta().y()?e->angleDelta().y():e->pixelDelta().y();
        if(delta) emit zoomed(delta>0?.8:1.25);
        e->accept();
    }
    void mousePressEvent(QMouseEvent* e) override {
        if(e->button()==Qt::LeftButton) {pressed_=last_=e->position();drag_=true;moved_=false;setCursor(Qt::ClosedHandCursor);}
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if(!drag_) return;
        if(!moved_&&(e->position()-pressed_).manhattanLength()<=4) return;
        const auto delta=e->position()-last_;last_=e->position();moved_=true;
        emit panned({-delta.x()/width(),delta.y()/height()});
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if(e->button()!=Qt::LeftButton) return;
        if(drag_&&!moved_) emit clicked({e->position().x()/width(),e->position().y()/height()});
        drag_=false;setCursor(Qt::OpenHandCursor);
    }
    void contextMenuEvent(QContextMenuEvent* e) override {
        emit menuRequested(e->reason()==QContextMenuEvent::Keyboard?rect().center():e->pos());e->accept();
    }
    void keyPressEvent(QKeyEvent* e) override {
        switch(e->key()) {
        case Qt::Key_Left:emit panned({-.2,0});break;
        case Qt::Key_Right:emit panned({.2,0});break;
        case Qt::Key_Up:emit panned({0,.2});break;
        case Qt::Key_Down:emit panned({0,-.2});break;
        case Qt::Key_Plus:case Qt::Key_Equal:emit zoomed(.8);break;
        case Qt::Key_Minus:emit zoomed(1.25);break;
        default:QWidget::keyPressEvent(e);return;
        }
        e->accept();
    }
private:
    QImage frame_;QRectF frameView_,view_;QColor background_{"#F2F3F4"};
    QString notice_;QPointF pressed_,last_;bool drag_=false,moved_=false;
};
struct Prepared {renderer::RenderResult render;double totalMs=0;QImage tile;QString tileKey;};
Prepared draw(const app::WorkflowResult& state,std::shared_ptr<const app::RoadNetwork> projection,
    quint64 roadsRevision,QRectF view,QSize size,MapColors colors,
    std::optional<QPointF> start,std::optional<QPointF> end,QImage cache,QString key,QImage previousTile,QString previousTileKey,QImage pyramidBase,QRectF pyramidView) {
    QElapsedTimer timer;timer.start();Prepared out;renderer::MapScene scene;
    scene.logicalSize=size;scene.source=renderer::MapSource::Real;
    scene.dataVersion=projection?projection->version:"empty";
    auto screen=[&](QPointF geo){return screenPoint(mapProject(geo),view,size);};
    const auto tileKey=scene.dataVersion+QString("/%1/%2/%3/%4/%5/%6/%7x%8")
        .arg(roadsRevision).arg(view.left(),0,'g',17).arg(view.top(),0,'g',17)
        .arg(view.width(),0,'g',17).arg(view.height(),0,'g',17).arg(colors.land.name()).arg(size.width()).arg(size.height());
    QImage base;
    if(previousTileKey==tileKey&&previousTile.size()==size) base=previousTile;
    else {
        base=reprojectMapFrame(pyramidBase,pyramidView,view,size,colors.land);
        if(!projection){QPainter p(&base);p.setPen(colors.label);p.drawText(base.rect(),Qt::AlignCenter,"从菜单选择已导入地图\n拖动浏览 · 滚轮缩放 · 右键设置起终点");}
    }
    out.tile=base;out.tileKey=tileKey;
    scene.tiles.append({tileKey,QRectF(QPointF(),size),base,0,tileKey,renderer::MapSource::Real});
    auto line=[&](const QVector<QPointF>& points,const QString& id,QColor color,double width,int z) {
        if(!projection||points.size()<2) return;
        renderer::RouteLine route;route.id=id;route.source=renderer::MapSource::Real;route.dataVersion=scene.dataVersion;
        route.color=color;route.width=width;route.zOrder=z;
        for(const auto& pt:points) route.points.append(screen(pt));
        scene.routes.append(route);
    };
    line(state.observations,"trajectory",colors.track,2,10);
    line(state.route,"astar",colors.route,6,12);
    line(state.matched,"fmm",QColor("#C16A1A"),5,14);
    if(projection&&start) scene.markers.append({"start",screen(*start),renderer::MarkerKind::Start,QColor("#23875B"),"A",10,20});
    if(projection&&end) scene.markers.append({"end",screen(*end),renderer::MarkerKind::End,QColor("#D64F50"),"B",10,20});
    out.render=renderer::LinuxRenderer::render(scene,nullptr,size,renderer::RenderMode::CachedBase,0,cache,key);
    if(!out.render.frame.isNull()&&projection){
        QPainter p(&out.render.frame);p.setPen(QPen(colors.label,2));
        const auto center=mapUnproject(view.center());const double metresPerPixel=view.width()/size.width()*std::cos(center.y()*std::acos(-1.)/180.);
        const double target=metresPerPixel*100;const double unit=std::pow(10.,std::floor(std::log10(std::max(.001,target))));double distance=unit;
        for(double step:{1.,2.,5.})if(step*unit<=target)distance=step*unit;
        const double width=distance/metresPerPixel;
        p.drawLine(QPointF(20,size.height()-38),QPointF(20+width,size.height()-38));
        p.drawText(QPointF(20,size.height()-48),distance>=1000?QString("%1 km").arg(distance/1000):QString("%1 m").arg(distance));
        p.drawText(QPointF(20,size.height()-16),"© OpenStreetMap contributors · 本地分级地图");
    }
    out.totalMs=timer.nsecsElapsed()/1e6;return out;
}
class MapPage final : public QWidget {
    Q_OBJECT
public:
    MapPage(Theme& theme,app::NavigationWorkflow* flow,QString directory,QWidget* parent)
        :QWidget(parent),theme_(theme),flow_(flow),directory_(std::move(directory)),pyramid_(this,directory_.isEmpty()?QString():QDir(directory_).filePath("map-pyramids")) {
        setObjectName("mapPage");auto* layout=new QGridLayout(this);layout->setContentsMargins(0,0,0,0);
        canvas_=new MapCanvas;layout->addWidget(canvas_,0,0);
        auto* tools=new QWidget;tools->setObjectName("mapFloatingTools");
        auto* buttons=new QHBoxLayout(tools);buttons->setContentsMargins(16,16,16,0);buttons->setSpacing(8);
        auto* menuButton=button("菜单");menuButton->setObjectName("mapMenuButton");menuButton->setAccessibleName("地图菜单");buttons->addWidget(menuButton);
        plan_=button("计算路线",true);plan_->setObjectName("calculateRoute");buttons->addWidget(plan_);plan_->hide();
        layout->addWidget(tools,0,0,Qt::AlignTop|Qt::AlignLeft);
        menu_=new QMenu(this);menu_->setObjectName("mapMainMenu");menuButton->setMenu(menu_);
        datasets_=menu_->addMenu("选择已导入地图");datasets_->setObjectName("mapDatasetsMenu");
        connect(menu_->addAction("导入地图…"),&QAction::triggered,this,[this]{emit pageRequested("import");});
        connect(menu_->addAction("算法与详细信息…"),&QAction::triggered,this,[this]{emit pageRequested("algorithms");});
        menu_->addSeparator();
        auto* fitAction=menu_->addAction("显示当前区域");fitAction->setObjectName("fitRoadNetwork");connect(fitAction,&QAction::triggered,this,&MapPage::fit);
        auto* results=menu_->addAction("查看路线");results->setObjectName("fitNavigationResult");connect(results,&QAction::triggered,this,&MapPage::fitResults);
        connect(menu_->addAction("放大　+"),&QAction::triggered,this,[this]{zoom(.8);});
        connect(menu_->addAction("缩小　−"),&QAction::triggered,this,[this]{zoom(1.25);});
        connect(menu_->addAction("清除起终点与路线"),&QAction::triggered,flow_,&app::NavigationWorkflow::clearEndpoints);
        cancel_=menu_->addAction("取消当前计算");connect(cancel_,&QAction::triggered,flow_,&app::NavigationWorkflow::cancel);
        menu_->addSeparator();onlyMap_=menu_->addAction("仅显示地图　Ctrl+M");onlyMap_->setCheckable(true);
        connect(onlyMap_,&QAction::triggered,this,&MapPage::toggleMapOnlyRequested);
        connect(menu_->addAction("切换明暗主题"),&QAction::triggered,this,[this]{theme_.setDark(!theme_.isDark());});
        connect(menu_,&QMenu::aboutToShow,this,[this]{onlyMap_->setChecked(window()->property("mapOnly").toBool());refreshDatasets();});
        connect(plan_,&QPushButton::clicked,this,&MapPage::plan);
        auto* planShortcut=new QShortcut(QKeySequence("Ctrl+Return"),this);planShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(planShortcut,&QShortcut::activated,this,&MapPage::plan);
        connect(canvas_,&MapCanvas::zoomed,this,&MapPage::zoom);
        connect(canvas_,&MapCanvas::panned,this,[this](QPointF delta) {
            if(!projection_) return;
            const auto actual=aspectView(view_,canvas_->size());view_.translate(delta.x()*actual.width(),delta.y()*actual.height());
            clampView();schedule();regionTimer_.start();
        });
        connect(canvas_,&MapCanvas::clicked,this,[this](QPointF f){if(projection_) flow_->acceptMapPoint(geoPoint(f));});
        connect(canvas_,&MapCanvas::menuRequested,this,&MapPage::showPointMenu);
        connect(canvas_,&MapCanvas::resized,this,[this]{clampView();schedule();regionTimer_.start();});
        connect(flow_,&app::NavigationWorkflow::changed,this,&MapPage::sync);
        connect(&pyramid_,&TilePyramid::statusChanged,this,[this](const QString& text){regionStatus_=text;updateNotice();});
        connect(&pyramid_,&TilePyramid::frameReady,this,[this](QImage image,QRectF view,PyramidStats stats){
            pyramidBase_=image;pyramidView_=view;++pyramidRevision_;
            regionStatus_=stats.roads?QString():QString("当前范围没有已导入道路");updateNotice();
            canvas_->setProperty("tileZoom",stats.zoom);canvas_->setProperty("tileCount",stats.tiles);
            canvas_->setProperty("roadCount",stats.roads);
            canvas_->setProperty("tileCacheHits",stats.hits);canvas_->setProperty("parentTiles",stats.parents);
            schedule();
        });
        connect(&theme_,&Theme::changed,this,[this]{pyramidBase_={};schedule();regionTimer_.start();});
        debounce_.setSingleShot(true);debounce_.setInterval(30);connect(&debounce_,&QTimer::timeout,this,&MapPage::render);
        regionTimer_.setSingleShot(true);regionTimer_.setInterval(180);connect(&regionTimer_,&QTimer::timeout,this,&MapPage::loadVisible);
        noticeTimer_.setSingleShot(true);noticeTimer_.setInterval(6000);connect(&noticeTimer_,&QTimer::timeout,this,[this]{workflowStatus_.clear();updateNotice();});
        sync();refreshDatasets();schedule();
    }
    ~MapPage() override {
        if(watcher_) watcher_->waitForFinished();
        if(catalog_) catalog_->waitForFinished();
    }
signals:
    void framePublished();
    void pageRequested(const QString& id);
    void toggleMapOnlyRequested();
private:
    QPointF geoPoint(QPointF fraction) const {
        const auto actual=aspectView(view_,canvas_->size());
        return mapUnproject({actual.left()+fraction.x()*actual.width(),actual.bottom()-fraction.y()*actual.height()});
    }
    void updateNotice() {canvas_->setNotice(!regionStatus_.isEmpty()?regionStatus_:workflowStatus_);}
    void sync() {
        if(flow_->state().network&&sourceRevision_!=flow_->datasetRevision()) {
            sourceRevision_=flow_->datasetRevision();projection_=flow_->state().network;
            pyramid_.setDataset(*flow_->activeDataset(),projection_->roads);pyramidBase_={};regionStatus_.clear();fit();
        }
        plan_->setVisible(flow_->startPoint().has_value()&&flow_->endPoint().has_value());
        plan_->setEnabled(!flow_->busy());cancel_->setEnabled(flow_->busy());
        if(lastStatus_!=flow_->status()) {
            lastStatus_=flow_->status();workflowStatus_=lastStatus_;noticeTimer_.stop();
            if(!flow_->busy()&&flow_->lastResult().ok) noticeTimer_.start();
            updateNotice();
        }
        schedule();
    }
    void plan() {if(!flow_->busy()&&flow_->startPoint()&&flow_->endPoint()) flow_->plan(*flow_->startPoint(),*flow_->endPoint());}
    void showPointMenu(QPoint position) {
        if(!projection_) return;
        const QPointF point=geoPoint({double(position.x())/canvas_->width(),double(position.y())/canvas_->height()});
        if(std::abs(point.x())>180||std::abs(point.y())>85) return;
        auto* menu=new QMenu(canvas_);menu->setObjectName("mapPointMenu");menu->setAttribute(Qt::WA_DeleteOnClose);
        auto* start=menu->addAction("设为起点");start->setObjectName("setMapStart");
        auto* end=menu->addAction("设为终点");end->setObjectName("setMapEnd");
        connect(start,&QAction::triggered,this,[this,point]{flow_->setEndpoint(true,point);});
        connect(end,&QAction::triggered,this,[this,point]{flow_->setEndpoint(false,point);});
        menu->addSeparator();auto* route=menu->addAction("计算路线");route->setObjectName("planMapRoute");
        route->setEnabled(!flow_->busy()&&flow_->startPoint()&&flow_->endPoint());connect(route,&QAction::triggered,this,&MapPage::plan);
        auto* clear=menu->addAction("清除起终点与路线");connect(clear,&QAction::triggered,flow_,&app::NavigationWorkflow::clearEndpoints);
        menu->popup(canvas_->mapToGlobal(position));
    }
    void clampView() {
        if(!projection_||!view_.isValid()) return;
        const auto actual=aspectView(view_,canvas_->size());
        const double aspect=double(std::max(1,canvas_->width()))/std::max(1,canvas_->height());
        const double maximum=40075016.68557849*std::min(1.,aspect);
        if(actual.width()>maximum) {const auto center=actual.center();view_=QRectF(center-QPointF(maximum/2,maximum/aspect/2),QSizeF(maximum,maximum/aspect));}
        const auto current=aspectView(view_,canvas_->size());const double half=40075016.68557849/2;
        const QPointF center(std::clamp(current.center().x(),-half+current.width()/2,half-current.width()/2),
                             std::clamp(current.center().y(),-half+current.height()/2,half-current.height()/2));
        view_.translate(center-current.center());
    }

    void zoom(double factor) {
        if(!projection_) return;
        const auto center=view_.center();
        const double width=std::clamp(view_.width()*factor,50.,100000.);const double height=width*view_.height()/view_.width();
        view_=QRectF(center-QPointF(width/2,height/2),QSizeF(width,height));clampView();schedule();regionTimer_.start();
    }
    void fit() {
        if(!projection_) return;
        const auto bounds=projection_->roads->bounds();
        view_=QRectF(mapProject({bounds.minLon,bounds.minLat}),mapProject({bounds.maxLon,bounds.maxLat})).normalized();
        if(view_.width()<100) view_.adjust(-50,0,50,0);
        if(view_.height()<100) view_.adjust(0,-50,0,50);
        view_.adjust(-view_.width()*.05,-view_.height()*.05,view_.width()*.05,view_.height()*.05);
        clampView();schedule();regionTimer_.start();
    }
    void fitResults() {
        if(!projection_) return;
        QVector<QPointF> points=flow_->state().route;points+=flow_->state().matched;
        if(points.isEmpty()) return;
        const auto first=mapProject(points.front());double left=first.x(),right=left,top=first.y(),bottom=top;
        for(const auto& geo:points) {const auto p=mapProject(geo);left=std::min(left,p.x());right=std::max(right,p.x());top=std::min(top,p.y());bottom=std::max(bottom,p.y());}
        const double padding=std::max(80.,std::max(right-left,bottom-top)*.2);
        view_=QRectF(QPointF(left-padding,top-padding),QPointF(right+padding,bottom+padding));clampView();schedule();regionTimer_.start();
    }
    void loadVisible() {
        if(!projection_) return;
        const auto actual=aspectView(view_,canvas_->size());
        pyramid_.request(actual,canvas_->size(),theme_.isDark());
    }
    void refreshDatasets() {
        if(catalog_) return;
        auto* watcher=new QFutureWatcher<QVector<app::ImportResult>>(this);catalog_=watcher;
        connect(watcher,&QFutureWatcher<QVector<app::ImportResult>>::finished,this,[this,watcher] {
            const auto entries=watcher->result();catalog_=nullptr;watcher->deleteLater();datasets_->clear();
            if(entries.isEmpty()) {auto* empty=datasets_->addAction("暂无地图，请先导入");empty->setEnabled(false);}
            for(const auto& entry:entries) {
                auto* action=datasets_->addAction(entry.fileName);action->setToolTip(entry.sourceFormat);
                action->setCheckable(true);action->setChecked(flow_->activeDataset()&&flow_->activeDataset()->cachePath==entry.cachePath);
                connect(action,&QAction::triggered,this,[this,entry] {
                    double lat=(entry.minLat+entry.maxLat)/2,lon=(entry.minLon+entry.maxLon)/2;
                    if(entry.hasBounds&&entry.minLat<=30.593&&entry.maxLat>=30.593&&entry.minLon<=114.305&&entry.maxLon>=114.305) {lat=30.593;lon=114.305;}
                    if(projection_) {const auto center=geoPoint({.5,.5});
                        if(entry.hasBounds&&center.x()>=entry.minLon&&center.x()<=entry.maxLon&&center.y()>=entry.minLat&&center.y()<=entry.maxLat) {lat=center.y();lon=center.x();}}
                    flow_->load(entry,lat,lon,1.);
                });
            }
        });
        const auto directory=directory_;
        watcher->setFuture(QtConcurrent::run([directory] {
            QVector<app::ImportResult> all;const auto root=directory.isEmpty()?data::defaultImportCacheDirectory():directory;
            for(const auto& f:QDir(root).entryInfoList({"*.sqlite"},QDir::Files,QDir::Time)) {
                try {all.append(data::ImportedDataset(f.absoluteFilePath()).summary());}catch(const std::exception&) {}
            }
            all+=data::OsmxCache::list(directory.isEmpty()?data::OsmxCache::defaultDirectory():directory).entries;return all;
        }));
    }
    void schedule() {
        ++revision_;
        canvas_->setViewport(aspectView(view_.isValid()?view_:QRectF(-500,-500,1000,1000),canvas_->size()),mapColors(theme_.isDark()).land);
        if(!debounce_.isActive()) debounce_.start();
    }
    void render() {
        if(watcher_) {pending_=true;return;}if(canvas_->width()<2||canvas_->height()<2) return;
        const auto revision=revision_;const auto state=flow_->state();const auto size=canvas_->size();
        const auto view=aspectView(view_.isValid()?view_:QRectF(-500,-500,1000,1000),size);
        // Test/diagnostic properties do not add visible performance panels.
        if(projection_) {const auto center=mapUnproject(view.center());
            canvas_->setProperty("centerLongitude",center.x());canvas_->setProperty("centerLatitude",center.y());
            canvas_->setProperty("viewWidthMetres",view.width());canvas_->setProperty("viewHeightMetres",view.height());}
        const auto colors=mapColors(theme_.isDark());const auto image=cache_;const auto key=key_;const auto tile=tile_;const auto tileKey=tileKey_;
        const auto projection=projection_;const auto roadsRevision=pyramidRevision_;
        const auto pyramidBase=pyramidBase_;const auto pyramidView=pyramidView_;
        const auto start=flow_->startPoint(),end=flow_->endPoint();
        auto* watcher=new QFutureWatcher<Prepared>(this);watcher_=watcher;
        connect(watcher,&QFutureWatcher<Prepared>::finished,this,[this,watcher,revision,view] {
            const auto result=watcher->result();watcher_=nullptr;watcher->deleteLater();
            if(revision==revision_) {
                if(result.render.error.isEmpty()) {
                    canvas_->setFrame(result.render.frame,view);cache_=result.render.baseCache;key_=result.render.baseCacheKey;tile_=result.tile;tileKey_=result.tileKey;
                    canvas_->setProperty("realDataset",bool(projection_));
                    const auto& state=flow_->state();const int count=int(state.route.size()>1)+int(state.observations.size()>1)+int(state.matched.size()>1);
                    canvas_->setProperty("routeCount",count);canvas_->setProperty("markerCount",int(bool(flow_->startPoint()))+int(bool(flow_->endPoint())));
                    canvas_->setProperty("regionRevision",pyramidRevision_);
                    canvas_->setProperty("cacheHit",result.render.stats.cacheHit);canvas_->setProperty("renderElapsedMs",result.totalMs);emit framePublished();
                } else canvas_->setNotice(result.render.error);
            }
            if(pending_||revision!=revision_) {pending_=false;debounce_.start();}
        });
        watcher->setFuture(QtConcurrent::run([state,projection,roadsRevision,view,size,colors,start,end,image,key,tile,tileKey,pyramidBase,pyramidView] {
            return draw(state,projection,roadsRevision,view,size,colors,start,end,image,key,tile,tileKey,pyramidBase,pyramidView);
        }));
    }
    Theme& theme_;app::NavigationWorkflow* flow_;QString directory_;
    MapCanvas* canvas_;QPushButton* plan_;QMenu *menu_,*datasets_;QAction *onlyMap_,*cancel_;
    TilePyramid pyramid_;QImage pyramidBase_;QRectF pyramidView_;quint64 pyramidRevision_=0;
    std::shared_ptr<const app::RoadNetwork> projection_;
    QRectF view_;QTimer debounce_,regionTimer_,noticeTimer_;
    quint64 revision_=0,sourceRevision_=0;bool pending_=false;
    QFutureWatcher<Prepared>* watcher_=nullptr;QFutureWatcher<QVector<app::ImportResult>>* catalog_=nullptr;
    QImage cache_,tile_;QString key_,tileKey_,regionStatus_,workflowStatus_,lastStatus_;
};
}
QWidget* createMapPage(Theme& theme,QWidget* parent) {
    auto* flow=new app::NavigationWorkflow(parent);auto* page=new MapPage(theme,flow,{},parent);
    if(!parent) flow->setParent(page);
    return page;
}
QWidget* createMapPage(Theme& theme,app::NavigationWorkflow* flow,const QString& cache,QWidget* parent) {
    return new MapPage(theme,flow,cache,parent);
}
}
#include "MapView.moc"
