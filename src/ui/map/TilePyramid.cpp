#include "TilePyramid.h"
#include "MapFrame.h"
#include "ui/common/Theme.h"
#include "pal/linux/renderer/RoadPainter.h"
#include "data/import/Sqlite.h"
#include <QtConcurrent>
#include <QCache>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDirIterator>
#include <QFileInfo>
#include <QLockFile>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QLineF>
#include <cmath>
#include <algorithm>

namespace p1::ui {
namespace {
constexpr double world=40075016.68557849;
constexpr int pixels=256, minimumZoom=0,maximumZoom=18;
constexpr qint64 diskLimit=2LL*1024*1024*1024;
void check(const std::shared_ptr<std::atomic_bool>& c){if(c->load())throw std::runtime_error("已取消");}
QString hash(const QByteArray& data){return QString::fromLatin1(QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex());}
QRectF tileRect(int z,int x,int y){double step=world/std::ldexp(1.,z);return {-world/2+x*step,world/2-(y+1)*step,step,step};}
QPoint tileAt(QPointF p,int z){double n=std::ldexp(1.,z);return {std::clamp(int(std::floor((p.x()/world+.5)*n)),0,int(n)-1),std::clamp(int(std::floor((.5-p.y()/world)*n)),0,int(n)-1)};}
}
QPointF mapProject(QPointF g){const double lat=std::clamp(g.y(),-85.05112878,85.05112878);return {g.x()/360.*world,std::log(std::tan(std::acos(-1.)/4+lat*std::acos(-1.)/360))*world/(2*std::acos(-1.))};}
QPointF mapUnproject(QPointF p){return {p.x()/world*360,(2*std::atan(std::exp(p.y()/world*2*std::acos(-1.)))-std::acos(-1.)/2)*180/std::acos(-1.)};}

class TileBackend {
public:
    app::ImportResult entry;std::shared_ptr<const data::OsmxDataset> roads;
    QString directory;QCache<QString,QImage> memory{128*1024};bool ready=false;
    QImage lastFrame;QRectF lastView;bool lastDark=false;
    struct Warm {int z,x,y;bool dark;};QVector<Warm> warm;int nextWarm=0;
    TileBackend(app::ImportResult e,std::shared_ptr<const data::OsmxDataset> ds,QString root):entry(std::move(e)),roads(std::move(ds)) {
        // Versioned namespaces isolate dataset, coordinate scheme and styles.
        const auto identity=entry.sourceSha256+"/"+entry.datasetId+"/"+entry.cachePath+"/"+QString::number(QFileInfo(entry.cachePath).lastModified().toMSecsSinceEpoch());
        directory=QDir(root).filePath(hash(identity.toUtf8())+"/xyz-v2-areas1");
    }
    void ensure(const std::shared_ptr<std::atomic_bool>& token) {
        if(ready)return;
        check(token);
        if(!QDir().mkpath(directory))throw std::runtime_error("无法创建地图缓存目录，请检查目录权限和磁盘空间");
        QLockFile lock(directory+"/build.lock");
        while(!lock.tryLock(100)){check(token);if(lock.error()!=QLockFile::LockFailedError)throw std::runtime_error("无法锁定地图索引缓存，请检查目录权限");}
        const QString path=directory+"/geometry.sqlite";
        if(QFileInfo::exists(path)) {
            try{data::detail::Db db(path,true);data::detail::Statement s(db,"SELECT value FROM metadata WHERE key='complete'");if(s.row()&&s.text(0)=="1")ready=true;}catch(const std::exception&){}
        }
        if(!ready){
            const QString temporary=directory+"/geometry.build.sqlite";QFile::remove(temporary);
            try{
                {data::detail::Db db(temporary);db.exec("PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; BEGIN; CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT); CREATE TABLE roads(id INTEGER PRIMARY KEY,z INTEGER,name TEXT,geometry BLOB); CREATE VIRTUAL TABLE bounds USING rtree(id,minx,maxx,miny,maxy);");
                data::detail::Statement insert(db,"INSERT INTO roads VALUES(?,?,?,?)"),bounds(db,"INSERT INTO bounds VALUES(?,?,?,?,?)");qint64 id=0;
                data::visitMapRoads(entry,roads,[&](const data::MapRoad& r){check(token);QByteArray bytes;QDataStream stream(&bytes,QIODevice::WriteOnly);stream.setVersion(QDataStream::Qt_6_4);QRectF box;bool first=true;
                    QVector<QPointF> points;points.reserve(r.points.size());for(auto geo:r.points){const auto p=mapProject(geo);points.append(p);if(first){box=QRectF(p,QSizeF(0,0));first=false;}else{box=QRectF(QPointF(std::min(box.left(),p.x()),std::min(box.top(),p.y())),QPointF(std::max(box.right(),p.x()),std::max(box.bottom(),p.y())));}}
                    stream<<points;insert.integer(1,++id);insert.integer(2,r.minZoom);insert.text(3,r.name);
                    data::detail::error(db.value,sqlite3_bind_blob(insert.value,4,bytes.constData(),int(bytes.size()),SQLITE_TRANSIENT));insert.insert();
                    bounds.integer(1,id);bounds.real(2,box.left());bounds.real(3,box.right());bounds.real(4,box.top());bounds.real(5,box.bottom());bounds.insert();
                },token);
                db.exec("CREATE TABLE areas(id INTEGER PRIMARY KEY,kind INTEGER,z INTEGER,geometry BLOB); CREATE VIRTUAL TABLE area_bounds USING rtree(id,minx,maxx,miny,maxy);");
                data::detail::Statement areaInsert(db,"INSERT INTO areas VALUES(?,?,?,?)"),areaBounds(db,"INSERT INTO area_bounds VALUES(?,?,?,?,?)");qint64 areaId=0;
                const auto areaStats=data::visitMapAreas(entry,roads,[&](const data::MapArea& a){
                    check(token);QVector<QPointF> outer;QVector<QVector<QPointF>> holes;double minx=1e100,miny=1e100,maxx=-1e100,maxy=-1e100;
                    for(auto geo:a.outer){auto p=mapProject(geo);outer.append(p);minx=std::min(minx,p.x());maxx=std::max(maxx,p.x());miny=std::min(miny,p.y());maxy=std::max(maxy,p.y());}
                    for(const auto& ring:a.holes){QVector<QPointF> h;for(auto geo:ring)h.append(mapProject(geo));holes.append(std::move(h));}
                    QByteArray bytes;QDataStream stream(&bytes,QIODevice::WriteOnly);stream.setVersion(QDataStream::Qt_6_4);stream<<outer<<holes;
                    areaInsert.integer(1,++areaId);areaInsert.integer(2,int(a.kind));areaInsert.integer(3,a.minZoom);
                    data::detail::error(db.value,sqlite3_bind_blob(areaInsert.value,4,bytes.constData(),int(bytes.size()),SQLITE_TRANSIENT));areaInsert.insert();
                    areaBounds.integer(1,areaId);areaBounds.real(2,minx);areaBounds.real(3,maxx);areaBounds.real(4,miny);areaBounds.real(5,maxy);areaBounds.insert();
                },token);
                data::detail::Statement meta(db,"INSERT INTO metadata VALUES(?,?)");meta.text(1,"areas");meta.text(2,QString::number(areaStats.emitted));meta.insert();meta.text(1,"skipped_areas");meta.text(2,QString::number(areaStats.skipped));meta.insert();
                db.exec("INSERT INTO metadata VALUES('complete','1'); COMMIT;");}
                check(token);QFile::remove(path);if(!QFile::rename(temporary,path))throw std::runtime_error("无法发布地图金字塔索引");
            }catch(...){QFile::remove(temporary);throw;}
            ready=true;
        }
        // Entire imported coverage, bounded work: low levels first. Large
        // extracts retain lazy tiles beyond the explicit warm-up budget.
        const auto b=entry.hasBounds?data::OsmxBounds{entry.minLat,entry.minLon,entry.maxLat,entry.maxLon,true}:(roads?roads->bounds():data::OsmxBounds{});
        if(b.valid)for(int z=8;z<=10;++z){auto a=tileAt(mapProject({b.minLon,b.maxLat}),z),c=tileAt(mapProject({b.maxLon,b.minLat}),z);
            for(int y=a.y();y<=c.y()&&warm.size()<4096;++y)for(int x=a.x();x<=c.x()&&warm.size()<4096;++x){warm.append({z,x,y,false});warm.append({z,x,y,true});}}
    }
    // Raster style changes do not invalidate the expensive geometry index.
    QString key(int z,int x,int y,bool dark)const{return QString("raster-style3/%1/%2/%3/%4.png").arg(dark?"dark":"light").arg(z).arg(x).arg(y);}
    QImage cached(int z,int x,int y,bool dark) {
        auto k=key(z,x,y,dark);if(auto* image=memory.object(k))return *image;
        const auto path=directory+"/"+k;QImage image(path);
        if(image.size()!=QSize(pixels,pixels))return {};
        memory.insert(k,new QImage(image),int(image.sizeInBytes()/1024));
        QFile file(path);if(file.open(QIODevice::ReadOnly))file.setFileTime(QDateTime::currentDateTimeUtc(),QFileDevice::FileModificationTime);
        return image;
    }
    QImage generate(int z,int x,int y,bool dark,const std::shared_ptr<std::atomic_bool>& token,int* roadCount=nullptr) {
        check(token);auto image=cached(z,x,y,dark);if(!image.isNull())return image;
        const auto rect=tileRect(z,x,y);const double margin=rect.width()*16/pixels;auto query=rect.adjusted(-margin,-margin,margin,margin);
        data::detail::Db db(directory+"/geometry.sqlite",true);
        sqlite3_progress_handler(db.value,1000,[](void* p){return static_cast<std::atomic_bool*>(p)->load()?1:0;},token.get());
        data::detail::Statement s(db,"SELECT r.z,r.name,r.geometry FROM bounds b JOIN roads r ON r.id=b.id WHERE b.minx<=? AND b.maxx>=? AND b.miny<=? AND b.maxy>=? AND r.z<=? ORDER BY r.z DESC,r.id");
        s.real(1,query.right());s.real(2,query.left());s.real(3,query.bottom());s.real(4,query.top());s.integer(5,z);
        image=QImage(pixels,pixels,QImage::Format_ARGB32_Premultiplied);auto colors=mapColors(dark);image.fill(colors.land);QPainter painter(&image);painter.setRenderHint(QPainter::Antialiasing);
        data::detail::Statement areas(db,"SELECT a.kind,a.geometry FROM area_bounds b JOIN areas a ON a.id=b.id WHERE b.minx<=? AND b.maxx>=? AND b.miny<=? AND b.maxy>=? AND a.z<=? ORDER BY a.kind,(b.maxx-b.minx)*(b.maxy-b.miny) DESC,a.id");
        areas.real(1,query.right());areas.real(2,query.left());areas.real(3,query.bottom());areas.real(4,query.top());areas.integer(5,z);
        while(areas.row()){
            check(token);auto* data=static_cast<const char*>(sqlite3_column_blob(areas.value,1));QByteArray bytes(data,sqlite3_column_bytes(areas.value,1));QDataStream stream(bytes);stream.setVersion(QDataStream::Qt_6_4);
            QVector<QPointF> outer;QVector<QVector<QPointF>> holes;stream>>outer>>holes;if(stream.status()!=QDataStream::Ok||outer.size()<4)throw std::runtime_error("面要素缓存损坏");
            QPainterPath path;path.setFillRule(Qt::OddEvenFill);auto ring=[&](const QVector<QPointF>& points){if(points.isEmpty())return;auto pixel=[&](QPointF p){return QPointF((p.x()-rect.left())/rect.width()*pixels,(rect.bottom()-p.y())/rect.height()*pixels);};path.moveTo(pixel(points.front()));for(int i=1;i<points.size();++i)path.lineTo(pixel(points[i]));path.closeSubpath();};ring(outer);for(const auto& h:holes)ring(h);
            auto kind=data::MapAreaKind(areas.integer(0));QColor fill=colors.landuse;if(kind==data::MapAreaKind::Green)fill=colors.green;else if(kind==data::MapAreaKind::Water)fill=colors.water;else if(kind==data::MapAreaKind::Building)fill=colors.building;
            painter.setBrush(fill);painter.setPen(kind==data::MapAreaKind::Building?QPen(colors.buildingEdge,.6):QPen(Qt::NoPen));painter.drawPath(path);
        }
        QVector<renderer::RoadStroke> strokes;QVector<QPair<QPointF,QString>> labels;
        while(s.row()){check(token);if(roadCount)++*roadCount;const auto* ptr=static_cast<const char*>(sqlite3_column_blob(s.value,2));QByteArray bytes(ptr,sqlite3_column_bytes(s.value,2));QDataStream stream(bytes);stream.setVersion(QDataStream::Qt_6_4);QVector<QPointF> pts;stream>>pts;
            if(stream.status()!=QDataStream::Ok||pts.size()<2)throw std::runtime_error("金字塔几何缓存损坏");
            auto pixel=[&](QPointF p){return QPointF((p.x()-rect.left())/rect.width()*pixels,(rect.bottom()-p.y())/rect.height()*pixels);};
            QPainterPath path(pixel(pts.front()));QPointF last=pixel(pts.front());
            for(int i=1;i<pts.size();++i){auto p=pixel(pts[i]);if(i==pts.size()-1||QLineF(last,p).length()>=.6){path.lineTo(p);last=p;}}
            const bool major=s.integer(0)<=11;double width=major?(z<12?1.5:5.):(z<15?1.5:2.);
            strokes.append({path,width,major?colors.majorRoad:colors.road});
            auto name=s.text(1);auto position=pixel(pts[pts.size()/2]);
            if(z>=15&&!name.isEmpty()&&labels.size()<4&&QRectF(16,16,224,224).contains(position))labels.append({position,name});
        }
        renderer::paintRoads(painter,strokes,colors.roadEdge);
        painter.setPen(colors.label);for(const auto& label:labels)painter.drawText(label.first,label.second);
        painter.end();check(token);
        const auto k=key(z,x,y,dark),path=directory+"/"+k;QDir().mkpath(QFileInfo(path).absolutePath());QSaveFile file(path);
        if(!file.open(QIODevice::WriteOnly)||!image.save(&file,"PNG")||!file.commit())throw std::runtime_error("地图瓦片缓存写入失败");
        memory.insert(k,new QImage(image),int(image.sizeInBytes()/1024));return image;
    }
    void trim() {
        struct File{QString path; qint64 size,time;};QVector<File> files;qint64 total=0;
        QDirIterator it(directory,{"*.png"},QDir::Files,QDirIterator::Subdirectories);
        while(it.hasNext()){it.next();const auto f=it.fileInfo();if(f.isSymLink())continue;files.append({f.absoluteFilePath(),f.size(),f.lastModified().toMSecsSinceEpoch()});total+=f.size();}
        if(total<=diskLimit)return;
        std::sort(files.begin(),files.end(),[](const File&a,const File&b){return a.time<b.time;});
        for(const auto& f:files){if(total<=diskLimit)break;if(QFile::remove(f.path))total-=f.size;}
    }
    void frame(QRectF view,QSize size,bool dark,const std::shared_ptr<std::atomic_bool>& token,const std::function<void(QImage,PyramidStats)>& publish) {
        ensure(token);int z=std::clamp(int(std::floor(std::log2(world*size.width()/view.width()/pixels))),minimumZoom,maximumZoom);
        QPoint a,c;do{a=tileAt({view.left(),view.bottom()},z);c=tileAt({view.right(),view.top()},z);if((c.x()-a.x()+1)*(c.y()-a.y()+1)<=128||z==minimumZoom)break;--z;}while(true);
        auto compose=[&](bool create){
            QImage out=reprojectMapFrame(lastDark==dark?lastFrame:QImage(),lastView,view,size,mapColors(dark).land);
            const QRectF retained=lastDark==dark&&!lastFrame.isNull()?mapFrameTargetRect(lastView,view,size):QRectF();
            QPainter p(&out);PyramidStats stats;stats.zoom=z;
            { data::detail::Db db(directory+"/geometry.sqlite",true);data::detail::Statement count(db,"SELECT count(*) FROM bounds WHERE minx<=? AND maxx>=? AND miny<=? AND maxy>=?");count.real(1,view.right());count.real(2,view.left());count.real(3,view.bottom());count.real(4,view.top());if(count.row())stats.roads=int(count.integer(0)); }
            for(int y=a.y();y<=c.y();++y)for(int x=a.x();x<=c.x();++x){check(token);auto image=cached(z,x,y,dark);const bool hit=!image.isNull();bool parentImage=false;QRectF source(0,0,pixels,pixels);
                if(!hit&&create)image=generate(z,x,y,dark,token);
                if(image.isNull())for(int parent=z-1;parent>=minimumZoom;--parent){int factor=1<<(z-parent);image=cached(parent,x/factor,y/factor,dark);if(!image.isNull()){source=QRectF((x%factor)*double(pixels)/factor,(y%factor)*double(pixels)/factor,double(pixels)/factor,double(pixels)/factor);parentImage=true;++stats.parents;break;}}
                const auto r=tileRect(z,x,y);QRectF target((r.left()-view.left())/view.width()*size.width(),(view.bottom()-r.bottom())/view.height()*size.height(),r.width()/view.width()*size.width(),r.height()/view.height()*size.height());
                if(!image.isNull()) {
                    p.save();
                    // A coarse placeholder must never erase retained detail.
                    if(parentImage&&retained.isValid()) {
                        QPainterPath clip;clip.addRect(target);const auto overlap=target.intersected(retained);
                        if(!overlap.isEmpty())clip.addRect(overlap);
                        p.setClipPath(clip);
                    }
                    p.drawImage(target,image,source);p.restore();
                }
                ++stats.tiles;if(hit)++stats.hits;
            }p.end();lastFrame=out;lastView=view;lastDark=dark;publish(out,stats);};
        compose(false);compose(true);trim();
    }
};

TilePyramid::TilePyramid(QObject* p,QString root):QObject(p),root_(std::move(root)){pool_.setMaxThreadCount(1);if(root_.isEmpty())root_=QStandardPaths::writableLocation(QStandardPaths::CacheLocation)+"/map-pyramids";}
TilePyramid::~TilePyramid(){if(cancel_)cancel_->store(true);pool_.waitForDone();}
void TilePyramid::setDataset(const app::ImportResult& e,std::shared_ptr<const data::OsmxDataset> ds){++generation_;if(cancel_)cancel_->store(true);pending_=false;backend_=std::make_shared<TileBackend>(e,std::move(ds),root_);}
void TilePyramid::request(QRectF v,QSize size,bool dark){if(!backend_||!v.isValid()||size.isEmpty())return;view_=v;size_=size;dark_=dark;++generation_;if(cancel_)cancel_->store(true);pending_=true;if(!watcher_)start();}
void TilePyramid::start(){
    pending_=false;const auto backend=backend_;const auto generation=generation_;const auto view=view_;const auto size=size_;const bool dark=dark_;
    auto token=std::make_shared<std::atomic_bool>(false);cancel_=token;auto* watcher=new QFutureWatcher<QString>(this);watcher_=watcher;emit statusChanged("正在准备分级地图…");
    connect(watcher,&QFutureWatcher<QString>::finished,this,[this,watcher,generation]{auto error=watcher->result();watcher_=nullptr;watcher->deleteLater();if(generation==generation_)emit statusChanged(error);if(pending_)start();});
    watcher->setFuture(QtConcurrent::run(&pool_,[this,backend,generation,view,size,dark,token]{try{
        backend->frame(view,size,dark,token,[this,generation,view,token](QImage image,PyramidStats stats){QMetaObject::invokeMethod(this,[this,generation,view,image,stats,token]{if(generation==generation_&&!token->load())emit frameReady(image,view,stats);},Qt::QueuedConnection);});
        // Visible work has priority. Idle pre-generation yields after each tile.
        while(backend->nextWarm<backend->warm.size()){check(token);const auto w=backend->warm[backend->nextWarm];backend->generate(w.z,w.x,w.y,w.dark,token);++backend->nextWarm;}
        backend->trim();return QString();
    }catch(const std::exception& e){return token->load()?QString():QString::fromUtf8(e.what());}}));
}
}
