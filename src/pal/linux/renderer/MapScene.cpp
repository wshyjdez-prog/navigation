#include "pal/linux/renderer/MapScene.h"

#include <QCryptographicHash>
#include <QtGlobal>

#include <algorithm>

namespace p1::renderer {
namespace {

QString number(qreal value) {
    return QString::number(value, 'g', 17);
}

QString pointText(const QPointF& point) {
    return number(point.x()) + QLatin1Char(',') + number(point.y());
}

} // namespace

const char* toString(MapSource source) {
    return source == MapSource::Mock ? "mock" : "real";
}

const char* toString(MarkerKind kind) {
    switch (kind) {
    case MarkerKind::Start: return "start";
    case MarkerKind::End: return "end";
    case MarkerKind::CurrentPosition: return "current";
    case MarkerKind::PointOfInterest: return "poi";
    }
    return "?";
}

SceneValidation validateScene(const MapScene& scene, const QSize& outputSize) {
    if (!outputSize.isValid() || outputSize.isEmpty()) {
        return {false, QStringLiteral("渲染输出尺寸无效")};
    }
    if (!scene.logicalSize.isValid() || scene.logicalSize.width() <= 0 ||
        scene.logicalSize.height() <= 0) {
        return {false, QStringLiteral("MapScene 逻辑尺寸无效")};
    }
    if (scene.dataVersion.trimmed().isEmpty()) {
        return {false, QStringLiteral("MapScene 缺少 dataVersion")};
    }

    for (const auto& tile : scene.tiles) {
        if (tile.id.trimmed().isEmpty()) {
            return {false, QStringLiteral("瓦片缺少 id")};
        }
        if (tile.image.isNull() || tile.image.width() <= 0 || tile.image.height() <= 0) {
            return {false, QStringLiteral("瓦片图片无效：%1").arg(tile.id)};
        }
        if (!tile.target.isValid() || tile.target.isEmpty()) {
            return {false, QStringLiteral("瓦片目标区域无效：%1").arg(tile.id)};
        }
    }

    for (const auto& route : scene.routes) {
        if (route.id.trimmed().isEmpty()) {
            return {false, QStringLiteral("路线缺少 id")};
        }
        if (route.points.size() < 2) {
            return {false, QStringLiteral("路线至少需要两个点：%1").arg(route.id)};
        }
        if (!qIsFinite(route.width) || route.width <= 0) {
            return {false, QStringLiteral("路线宽度无效：%1").arg(route.id)};
        }
        if (!route.color.isValid()) {
            return {false, QStringLiteral("路线颜色无效：%1").arg(route.id)};
        }
        for (const auto& point : route.points) {
            if (!qIsFinite(point.x()) || !qIsFinite(point.y())) {
                return {false, QStringLiteral("路线包含非有限坐标：%1").arg(route.id)};
            }
        }
    }

    for (const auto& marker : scene.markers) {
        if (marker.id.trimmed().isEmpty()) {
            return {false, QStringLiteral("标记缺少 id")};
        }
        if (!qIsFinite(marker.position.x()) || !qIsFinite(marker.position.y())) {
            return {false, QStringLiteral("标记位置无效：%1").arg(marker.id)};
        }
        if (!qIsFinite(marker.radius) || marker.radius <= 0 || !marker.color.isValid()) {
            return {false, QStringLiteral("标记样式无效：%1").arg(marker.id)};
        }
    }

    return {true, {}};
}

QString baseCacheKey(const MapScene& scene, const QSize& outputSize) {
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(scene.dataVersion.toUtf8());
    hash.addData(toString(scene.source));
    hash.addData(number(scene.logicalSize.width()).toLatin1());
    hash.addData(number(scene.logicalSize.height()).toLatin1());
    hash.addData(number(outputSize.width()).toLatin1());
    hash.addData(number(outputSize.height()).toLatin1());

    QVector<MapTile> tiles = scene.tiles;
    std::sort(tiles.begin(), tiles.end(), [](const MapTile& left, const MapTile& right) {
        if (left.zOrder != right.zOrder) return left.zOrder < right.zOrder;
        return left.id < right.id;
    });
    for (const auto& tile : tiles) {
        hash.addData(tile.id.toUtf8());
        hash.addData(tile.dataVersion.toUtf8());
        hash.addData(toString(tile.source));
        hash.addData(number(tile.target.x()).toLatin1());
        hash.addData(number(tile.target.y()).toLatin1());
        hash.addData(number(tile.target.width()).toLatin1());
        hash.addData(number(tile.target.height()).toLatin1());
        hash.addData(QByteArray::number(tile.image.width()));
        hash.addData(QByteArray::number(tile.image.height()));
        hash.addData(QByteArray::number(tile.image.cacheKey()));
    }
    return QString::fromLatin1(hash.result().toHex());
}

} // namespace p1::renderer
