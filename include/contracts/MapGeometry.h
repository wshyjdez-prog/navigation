#pragma once
#include "app/ImportTypes.h"
#include "data/osmx/OsmxDataset.h"
#include <QPointF>
#include <functional>
#include <atomic>
namespace p1::data {
struct MapRoad { QVector<QPointF> points; QString name; int minZoom=14; };
enum class MapAreaKind { Landuse, Green, Water, Building };
struct MapArea {
    MapAreaKind kind=MapAreaKind::Landuse;
    QVector<QPointF> outer;
    QVector<QVector<QPointF>> holes;
    QString name;
    int minZoom=10;
};
struct AreaReadStats { quint64 emitted=0, skipped=0; };
// Coordinates remain WGS84 lon/lat. Invalid or incomplete polygons are omitted.
AreaReadStats visitMapAreas(const app::ImportResult&,std::shared_ptr<const OsmxDataset>,
    const std::function<void(const MapArea&)>&,const std::shared_ptr<std::atomic_bool>&);
// Streaming map geometry, independent of the routing graph and its size limits.
// Callback executes on the caller's worker. Points are WGS84 lon/lat.
void visitMapRoads(const app::ImportResult&,std::shared_ptr<const OsmxDataset>,
    const std::function<void(const MapRoad&)>&,const std::shared_ptr<std::atomic_bool>&);
}
