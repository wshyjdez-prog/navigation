#pragma once
#include <QPainter>
#include <QPainterPath>
#include <QVector>

namespace p1::renderer {
struct RoadStroke {
    QPainterPath path;
    qreal width;
    QColor fill;
};

// Complete the casing layer before painting any road interior. Otherwise a
// later way's rounded casing becomes a visible bar/ring inside an earlier way.
inline void paintRoads(QPainter& painter,const QVector<RoadStroke>& roads,QColor edge) {
    painter.save();painter.setBrush(Qt::NoBrush);
    for(const auto& road:roads) {
        painter.setPen(QPen(edge,road.width+2,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        painter.drawPath(road.path);
    }
    for(const auto& road:roads) {
        painter.setPen(QPen(road.fill,road.width,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        painter.drawPath(road.path);
    }
    painter.restore();
}
}
