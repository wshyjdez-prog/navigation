#include "pal/linux/renderer/MockMapScene.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <array>

namespace p1::renderer {
namespace {

QImage makeTile(int column, int row) {
    constexpr int tileWidth = 256;
    constexpr int tileHeight = 320;
    QImage image(tileWidth, tileHeight, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(QStringLiteral("#EEF2EA")));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);

    if (column == 1 && row == 0) {
        painter.fillRect(QRectF(28, 24, 86, 62), QColor(QStringLiteral("#DDE8DC")));
    }
    if (column == 3 && row == 1) {
        QLinearGradient water(0, 214, 256, 318);
        water.setColorAt(0, QColor(QStringLiteral("#D4E3F2")));
        water.setColorAt(1, QColor(QStringLiteral("#BFD8EB")));
        painter.fillRect(QRectF(0, 214, 256, 106), water);
    }

    painter.setPen(QPen(QColor(QStringLiteral("#E1DCE8")), 1));
    painter.setBrush(QColor(QStringLiteral("#F7F4FA")));
    for (int y = 12; y < tileHeight; y += 66) {
        for (int x = 10; x < tileWidth; x += 74) {
            painter.drawRoundedRect(QRectF(x, y, 42, 28), 3, 3);
        }
    }

    painter.setPen(QPen(Qt::white, 27, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawLine(QPointF(-15, 240 - row * 35),
                     QPointF(tileWidth + 15, 118 + column * 7));
    painter.drawLine(QPointF(68 + column * 10, -12),
                     QPointF(142 + row * 24, tileHeight + 12));
    painter.setPen(QPen(QColor(QStringLiteral("#CFC7DB")), 1));
    painter.drawLine(QPointF(-15, 240 - row * 35),
                     QPointF(tileWidth + 15, 118 + column * 7));
    return image;
}

} // namespace

MapScene makeMockMapScene() {
    MapScene scene;
    scene.logicalSize = QSizeF(1024.0, 640.0);
    scene.dataVersion = QStringLiteral("mock-campus-v1");
    scene.source = MapSource::Mock;

    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 4; ++column) {
            scene.tiles.push_back({
                QStringLiteral("mock/%1/%2").arg(row).arg(column),
                QRectF(column * 256.0, row * 320.0, 256.0, 320.0),
                makeTile(column, row),
                -10,
                scene.dataVersion,
                MapSource::Mock
            });
        }
    }

    scene.routes.push_back({
        QStringLiteral("mock-route-main"),
        {
            QPointF(92, 518),
            QPointF(198, 438),
            QPointF(286, 462),
            QPointF(388, 342),
            QPointF(484, 300),
            QPointF(602, 258),
            QPointF(718, 196),
            QPointF(844, 162),
        },
        QColor(QStringLiteral("#2F7DDB")),
        7.0,
        10,
        scene.dataVersion,
        MapSource::Mock
    });

    scene.markers = {
        {
            QStringLiteral("start"),
            QPointF(92, 518),
            MarkerKind::Start,
            QColor(QStringLiteral("#2BB673")),
            QStringLiteral("A"),
            11.0,
            20
        },
        {
            QStringLiteral("end"),
            QPointF(844, 162),
            MarkerKind::End,
            QColor(QStringLiteral("#E64B41")),
            QStringLiteral("B"),
            11.0,
            20
        },
        {
            QStringLiteral("vehicle"),
            QPointF(484, 300),
            MarkerKind::CurrentPosition,
            QColor(QStringLiteral("#4335BC")),
            QStringLiteral("当前位置"),
            9.0,
            25
        }
    };
    return scene;
}

contracts::NavigationSnapshot makeMockNavigationSnapshot() {
    using namespace p1::contracts;
    NavigationSnapshot snapshot;
    snapshot.status = NavigationStatus::Navigating;
    snapshot.paused = false;
    snapshot.navigationId = 1;
    snapshot.currentRequestId = 1;
    snapshot.dataVersion = QStringLiteral("mock-campus-v1");
    snapshot.source = SourceKind::Mock;
    snapshot.updatedAtMs = 1767225600000LL;
    snapshot.remainingDistanceM = 820.0;
    snapshot.remainingDurationSec = 210.0;

    RoutePlanRef route;
    route.routeId = QStringLiteral("mock-route-main");
    route.dataVersion = snapshot.dataVersion;
    route.geometryRef = QStringLiteral("resolved-by-mock-scene");
    route.totalDistanceM = 1240.0;
    route.totalDurationSec = 310.0;
    route.source = SourceKind::Mock;
    route.requestId = snapshot.currentRequestId;
    snapshot.route = route;

    PositionSample position;
    position.coordinate = GeoCoordinate{
        CoordinateSystem::WGS84, 30.512345, 114.423456, std::nullopt
    };
    position.timestampMs = snapshot.updatedAtMs;
    position.speedMps = 8.6;
    position.bearingDeg = 68.0;
    position.horizontalAccuracyM = 5.0;
    position.source = SourceKind::Mock;
    position.seq = 12;
    position.sessionId = 1;
    position.dataVersion = snapshot.dataVersion;
    snapshot.currentPosition = position;

    GuidanceInstruction guidance;
    guidance.kind = GuidanceKind::TurnRight;
    guidance.text = QStringLiteral("前方 320 米右转");
    guidance.distanceToManeuverM = 320.0;
    guidance.index = 1;
    snapshot.guidance = guidance;
    snapshot.guidanceIndex = guidance.index;
    return snapshot;
}

} // namespace p1::renderer
