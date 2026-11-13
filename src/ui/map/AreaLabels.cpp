#include "AreaLabels.h"
#include "data/import/Sqlite.h"
#include <QFontMetricsF>
#include <QHash>
#include <QLineF>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <vector>

namespace p1::ui {
namespace {
constexpr double world=40075016.68557849;
double segmentDistanceSquared(QPointF p,QPointF a,QPointF b) {
    double x=a.x(),y=a.y(),dx=b.x()-x,dy=b.y()-y;
    if(dx!=0||dy!=0) {
        const double t=((p.x()-x)*dx+(p.y()-y)*dy)/(dx*dx+dy*dy);
        if(t>1){x=b.x();y=b.y();}else if(t>0){x+=dx*t;y+=dy*t;}
    }
    dx=p.x()-x;dy=p.y()-y;return dx*dx+dy*dy;
}
// Positive inside the area (outer minus holes), negative outside.
double signedDistance(QPointF p,const QVector<QPointF>& outer,const QVector<QVector<QPointF>>& holes) {
    bool inside=false;double best=std::numeric_limits<double>::infinity();
    auto ring=[&](const QVector<QPointF>& r) {
        for(int i=0,j=int(r.size())-1;i<r.size();j=i++) {
            const auto a=r[i],b=r[j];
            if((a.y()>p.y())!=(b.y()>p.y())&&p.x()<(b.x()-a.x())*(p.y()-a.y())/(b.y()-a.y())+a.x())inside=!inside;
            best=std::min(best,segmentDistanceSquared(p,a,b));
        }
    };
    ring(outer);for(const auto& h:holes)ring(h);
    return (inside?1:-1)*std::sqrt(best);
}
struct Cell {
    QPointF centre;double half,distance,potential;
    bool operator<(const Cell& o)const{return potential<o.potential;}
};
int kindRank(data::MapAreaKind k) {
    switch(k){case data::MapAreaKind::Water:return 0;case data::MapAreaKind::Green:return 1;case data::MapAreaKind::Landuse:return 2;default:return 3;}
}
QFont labelFont(data::MapAreaKind kind,bool road=false) {
    QFont font;font.setPixelSize(!road&&kind==data::MapAreaKind::Building?11:12);
    font.setItalic(!road&&kind==data::MapAreaKind::Water);font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}
}

std::pair<QPointF,double> poleOfInaccessibility(const QVector<QPointF>& outer,const QVector<QVector<QPointF>>& holes,int maxCells) {
    if(outer.isEmpty())return {{},0};
    double minx=outer.front().x(),maxx=minx,miny=outer.front().y(),maxy=miny;
    for(auto p:outer){minx=std::min(minx,p.x());maxx=std::max(maxx,p.x());miny=std::min(miny,p.y());maxy=std::max(maxy,p.y());}
    const double width=maxx-minx,height=maxy-miny;double size=std::min(width,height);
    if(!(size>0))return {outer.front(),0};
    // Very elongated rings would otherwise start with thousands of cells.
    if((width/size)*(height/size)>maxCells/4.)size=std::sqrt(width*height/(maxCells/4.));
    const double precision=std::max(std::min(width,height)/100.,1e-3);
    auto make=[&](QPointF c,double half){const double d=signedDistance(c,outer,holes);return Cell{c,half,d,d+half*std::sqrt(2.)};};
    std::priority_queue<Cell> queue;int evaluations=0;
    for(double x=minx;x<maxx;x+=size)for(double y=miny;y<maxy;y+=size){queue.push(make({x+size/2,y+size/2},size/2));++evaluations;}
    // Seed with the area centroid; fall back to the bounding-box centre.
    double area=0,cx=0,cy=0;
    for(int i=0,j=int(outer.size())-1;i<outer.size();j=i++){const auto a=outer[i]-outer.front(),b=outer[j]-outer.front();const double f=a.x()*b.y()-b.x()*a.y();cx+=(a.x()+b.x())*f;cy+=(a.y()+b.y())*f;area+=f*3;}
    Cell best=area!=0?make(outer.front()+QPointF(cx/area,cy/area),0):make({minx+width/2,miny+height/2},0);
    const auto box=make({minx+width/2,miny+height/2},0);if(box.distance>best.distance)best=box;
    while(!queue.empty()) {
        const auto cell=queue.top();queue.pop();
        if(cell.distance>best.distance)best=cell;
        if(cell.potential-best.distance<=precision||evaluations>=maxCells)continue;
        const double h=cell.half/2;
        for(auto offset:{QPointF(-h,-h),QPointF(h,-h),QPointF(-h,h),QPointF(h,h)}){queue.push(make(cell.centre+offset,h));++evaluations;}
    }
    return {best.centre,std::max(best.distance,0.)};
}

int areaLabelMinZoom(data::MapAreaKind kind,int areaMinZoom,double clearance) {
    if(!(clearance>0))return -1;
    // Building names are dense; keep them two levels behind their footprints.
    const int floorZoom=kind==data::MapAreaKind::Building?std::max(areaMinZoom,16):areaMinZoom;
    const int needed=int(std::ceil(std::log2(kAreaLabelClearancePixels*world/(256*clearance))-1e-9));
    const int z=std::max({0,floorZoom,needed});
    return z>kAreaLabelMaxZoom?-1:z;
}

std::optional<AreaLabel> makeAreaLabel(data::MapAreaKind kind,int areaMinZoom,const QString& name,
    const QVector<QPointF>& outer,const QVector<QVector<QPointF>>& holes) {
    const auto text=name.simplified();if(text.isEmpty()||outer.size()<4)return std::nullopt;
    const auto [anchor,clearance]=poleOfInaccessibility(outer,holes);
    const int z=areaLabelMinZoom(kind,areaMinZoom,clearance);if(z<0)return std::nullopt;
    AreaLabel label;label.kind=kind;label.name=text;label.anchor=anchor;label.clearance=clearance;label.minZoom=z;
    return label;
}

std::optional<AreaLabel> makeRoadLabel(int roadMinZoom,const QString& name,const QVector<QPointF>& points) {
    const auto text=name.simplified();if(text.isEmpty()||points.size()<2)return std::nullopt;
    double length=0;for(int i=1;i<points.size();++i)length+=QLineF(points[i-1],points[i]).length();
    if(!(length>0))return std::nullopt;
    QPointF anchor=points.back();double remaining=length/2;
    for(int i=1;i<points.size();++i){const QLineF segment(points[i-1],points[i]);if(remaining<=segment.length()){anchor=segment.pointAt(segment.length()>0?remaining/segment.length():0);break;}remaining-=segment.length();}
    AreaLabel label;label.road=true;label.name=text;label.anchor=anchor;label.clearance=length;label.minZoom=std::max(kRoadLabelMinZoom,roadMinZoom);
    if(label.minZoom>kAreaLabelMaxZoom)return std::nullopt;
    return label;
}

struct AreaLabelWriter::Impl {
    data::detail::Statement labels,bounds;qint64 id=0;
    explicit Impl(data::detail::Db& db):labels(db,"INSERT INTO map_labels VALUES(?,?,?,?,?,?,?,?)"),bounds(db,"INSERT INTO label_bounds VALUES(?,?,?,?,?)"){}
};
AreaLabelWriter::AreaLabelWriter(data::detail::Db& db) {
    db.exec("CREATE TABLE map_labels(id INTEGER PRIMARY KEY,road INTEGER NOT NULL,kind INTEGER NOT NULL,z INTEGER NOT NULL,weight REAL NOT NULL,name TEXT NOT NULL,x REAL NOT NULL,y REAL NOT NULL);"
            "CREATE VIRTUAL TABLE label_bounds USING rtree(id,minx,maxx,miny,maxy);");
    d_=std::make_unique<Impl>(db);
}
AreaLabelWriter::~AreaLabelWriter()=default;
qint64 AreaLabelWriter::count()const{return d_->id;}
void AreaLabelWriter::add(const AreaLabel& l) {
    auto& s=d_->labels;s.integer(1,++d_->id);s.integer(2,l.road?1:0);s.integer(3,int(l.kind));s.integer(4,l.minZoom);s.real(5,l.clearance);s.text(6,l.name);s.real(7,l.anchor.x());s.real(8,l.anchor.y());s.insert();
    auto& b=d_->bounds;b.integer(1,d_->id);b.real(2,l.anchor.x());b.real(3,l.anchor.x());b.real(4,l.anchor.y());b.real(5,l.anchor.y());b.insert();
}

QVector<AreaLabel> queryAreaLabels(data::detail::Db& db,QRectF r,int z) {
    data::detail::Statement s(db,"SELECT l.id,l.kind,l.z,l.weight,l.name,l.x,l.y,l.road FROM label_bounds b JOIN map_labels l ON l.id=b.id "
        "WHERE b.minx<=? AND b.maxx>=? AND b.miny<=? AND b.maxy>=? AND l.z<=?");
    s.real(1,r.right());s.real(2,r.left());s.real(3,r.bottom());s.real(4,r.top());s.integer(5,z);
    QVector<AreaLabel> out;
    while(s.row()){AreaLabel l;l.id=s.integer(0);const auto kind=s.integer(1);
        if(kind<int(data::MapAreaKind::Landuse)||kind>int(data::MapAreaKind::Building))throw std::runtime_error("地图标注缓存损坏");
        l.kind=data::MapAreaKind(kind);l.road=s.integer(7)!=0;l.minZoom=int(s.integer(2));l.clearance=s.real(3);l.name=s.text(4);l.anchor={s.real(5),s.real(6)};out.append(std::move(l));}
    return out;
}

qreal areaLabelQueryMarginPixels() {
    // A label reaching into the raster depends on candidates that overlap it
    // (one more label width out) or share its name within the merge radius.
    const qreal half=kAreaLabelMaxTextWidth/2+kAreaLabelPadding;
    return half+std::max(2*half,kSameNameRadiusPixels);
}

QVector<PlacedAreaLabel> placeAreaLabels(QVector<AreaLabelCandidate> candidates) {
    std::sort(candidates.begin(),candidates.end(),[](const AreaLabelCandidate& a,const AreaLabelCandidate& b){
        if(a.label.road!=b.label.road)return a.label.road;
        if(a.label.clearance!=b.label.clearance)return a.label.clearance>b.label.clearance;
        if(kindRank(a.label.kind)!=kindRank(b.label.kind))return kindRank(a.label.kind)<kindRank(b.label.kind);
        return a.label.id<b.label.id;});
    constexpr double cell=64;QHash<quint64,QVector<QRectF>> grid;QVector<PlacedAreaLabel> placed;
    auto cells=[&](const QRectF& r,const std::function<bool(quint64)>& visit){
        for(qint64 x=qint64(std::floor(r.left()/cell));x<=qint64(std::floor(r.right()/cell));++x)
            for(qint64 y=qint64(std::floor(r.top()/cell));y<=qint64(std::floor(r.bottom()/cell));++y)
                if(!visit((quint64(quint32(x))<<32)|quint32(y)))return false;
        return true;};
    std::vector<QFontMetricsF> metrics;
    for(auto kind:{data::MapAreaKind::Landuse,data::MapAreaKind::Green,data::MapAreaKind::Water,data::MapAreaKind::Building})metrics.emplace_back(labelFont(kind));
    metrics.emplace_back(labelFont(data::MapAreaKind::Landuse,true));
    QHash<QString,QVector<QPointF>> anchorsByName;const qreal radius2=kSameNameRadiusPixels*kSameNameRadiusPixels;
    for(const auto& c:candidates) {
        auto& sameName=anchorsByName[c.label.name];
        const bool duplicate=std::any_of(sameName.cbegin(),sameName.cend(),[&](QPointF p){const auto d=p-c.pixel;return d.x()*d.x()+d.y()*d.y()<radius2;});
        sameName.append(c.pixel);
        const auto& fm=metrics[c.label.road?metrics.size()-1:size_t(c.label.kind)];const auto text=fm.elidedText(c.label.name,Qt::ElideRight,kAreaLabelMaxTextWidth);
        if(text.isEmpty())continue;
        const qreal w=fm.horizontalAdvance(text)+2*kAreaLabelPadding,h=fm.height()+2*kAreaLabelPadding;
        const QRectF box(c.pixel.x()-w/2,c.pixel.y()-h/2,w,h);
        const bool free=!duplicate&&cells(box,[&](quint64 k){for(const auto& other:grid.value(k))if(other.intersects(box))return false;return true;});
        if(free)placed.append({box,text,c.label.kind,c.label.id,c.label.road});
        cells(box,[&](quint64 k){grid[k].append(box);return true;});
    }
    return placed;
}

void paintAreaLabels(QPainter& painter,const QVector<PlacedAreaLabel>& labels,const MapColors& colors) {
    painter.save();painter.setRenderHint(QPainter::Antialiasing);
    for(const auto& l:labels) {
        const auto font=labelFont(l.kind,l.road);const QFontMetricsF fm(font);QPainterPath path;
        path.addText(QPointF(l.box.left()+kAreaLabelPadding,l.box.top()+kAreaLabelPadding+fm.ascent()),font,l.text);
        painter.setBrush(Qt::NoBrush);painter.setPen(QPen(colors.labelHalo,3,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));painter.drawPath(path);
        painter.setPen(Qt::NoPen);painter.setBrush(!l.road&&l.kind==data::MapAreaKind::Water?colors.waterLabel:colors.label);painter.drawPath(path);
    }
    painter.restore();
}
}
