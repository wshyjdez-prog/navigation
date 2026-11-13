#pragma once
#include "contracts/MapGeometry.h"
#include "ui/common/Theme.h"
#include <QPainter>
#include <QRectF>
#include <memory>
#include <optional>
#include <utility>

namespace p1::data::detail { struct Db; }
namespace p1::ui {
// Map name labels (areas and roads) baked into the tile pyramid. All geometry
// is in Web Mercator metres, the same space as the pyramid index.
//
// Anchor: pole of inaccessibility, i.e. the interior point farthest from every
// ring edge (holes included), so concave or holed areas never get a label
// outside their own fill. Its distance to the nearest edge is the clearance.
// Level: a label appears from the later of the kind's floor and the first
// level where the clearance reaches kAreaLabelClearancePixels.
// Roads: anchor at half the way's length; shown from kRoadLabelMinZoom.
struct AreaLabel {
    qint64 id=0;
    data::MapAreaKind kind=data::MapAreaKind::Landuse; // unused for roads
    bool road=false;
    QString name;
    QPointF anchor;
    double clearance=0; // priority weight: area clearance / road length (m)
    int minZoom=0;
};
constexpr int kRoadLabelMinZoom=15;
// Same-name labels closer than this (anchor distance) merge into the one
// with the higher priority, e.g. a street split into many OSM ways.
constexpr qreal kSameNameRadiusPixels=256;
constexpr double kAreaLabelClearancePixels=10;
constexpr int kAreaLabelMaxZoom=18;
constexpr qreal kAreaLabelMaxTextWidth=160;
constexpr qreal kAreaLabelPadding=3;

// Polylabel-style grid refinement (algorithm after Agafonkin's "polylabel";
// independent implementation). Work is bounded by maxCells for huge rings.
std::pair<QPointF,double> poleOfInaccessibility(const QVector<QPointF>& outer,
    const QVector<QVector<QPointF>>& holes,int maxCells=1500);
// -1 when the area never becomes large enough before kAreaLabelMaxZoom.
int areaLabelMinZoom(data::MapAreaKind kind,int areaMinZoom,double clearanceMetres);
std::optional<AreaLabel> makeAreaLabel(data::MapAreaKind kind,int areaMinZoom,const QString& name,
    const QVector<QPointF>& outer,const QVector<QVector<QPointF>>& holes);
std::optional<AreaLabel> makeRoadLabel(int roadMinZoom,const QString& name,const QVector<QPointF>& points);

// Index persistence: table map_labels + point R-tree label_bounds.
class AreaLabelWriter {
public:
    explicit AreaLabelWriter(data::detail::Db& db); // creates the tables
    ~AreaLabelWriter();
    void add(const AreaLabel& label);
    qint64 count() const;
private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
// Labels whose anchor lies in worldRect and that are visible at zoom z.
QVector<AreaLabel> queryAreaLabels(data::detail::Db& db,QRectF worldRect,int z);

// Strict placement: a label is kept only when no higher-priority candidate
// (kept or not) overlaps it or carries the same name within
// kSameNameRadiusPixels. The result for one label depends only on its
// neighbours, so every tile that sees it draws it identically (seamless
// across tile borders). Priority: roads before areas; then length/clearance;
// then water/green/landuse/building; then id.
struct AreaLabelCandidate { AreaLabel label; QPointF pixel; };
struct PlacedAreaLabel { QRectF box; QString text; data::MapAreaKind kind; qint64 id; bool road=false; };
// Extra pixels around a raster whose anchors must be considered.
qreal areaLabelQueryMarginPixels();
QVector<PlacedAreaLabel> placeAreaLabels(QVector<AreaLabelCandidate> candidates);
void paintAreaLabels(QPainter& painter,const QVector<PlacedAreaLabel>& labels,const MapColors& colors);
}
