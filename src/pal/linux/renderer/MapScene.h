#pragma once

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVector>

namespace p1::renderer {

enum class MapSource {
    Mock,
    Real
};

enum class MarkerKind {
    Start,
    End,
    CurrentPosition,
    PointOfInterest
};

// 一张已经完成的栅格瓦片。坐标与大小使用 MapScene 的逻辑坐标。
struct MapTile {
    QString id;
    QRectF target;
    QImage image;
    int zOrder = 0;
    QString dataVersion;
    MapSource source = MapSource::Mock;
};

// 一条已经由 3 号或样例提供者转换好的屏幕坐标路线。
struct RouteLine {
    QString id;
    QVector<QPointF> points;
    QColor color = QColor(QStringLiteral("#2F7DDB"));
    qreal width = 7.0;
    int zOrder = 10;
    QString dataVersion;
    MapSource source = MapSource::Mock;
};

struct MapMarker {
    QString id;
    QPointF position;
    MarkerKind kind = MarkerKind::PointOfInterest;
    QColor color = QColor(QStringLiteral("#4335BC"));
    QString label;
    qreal radius = 10.0;
    int zOrder = 20;
};

// LinuxRenderer 的模块内输入契约。当前使用 Mock；2/3/4 号的数据通过适配层转换后填入。
struct MapScene {
    QSizeF logicalSize{1024.0, 640.0};
    QString dataVersion;
    MapSource source = MapSource::Mock;
    QVector<MapTile> tiles;
    QVector<RouteLine> routes;
    QVector<MapMarker> markers;
};

struct SceneValidation {
    bool valid = false;
    QString error;
};

SceneValidation validateScene(const MapScene& scene, const QSize& outputSize);
QString baseCacheKey(const MapScene& scene, const QSize& outputSize);
const char* toString(MapSource source);
const char* toString(MarkerKind kind);

} // namespace p1::renderer
