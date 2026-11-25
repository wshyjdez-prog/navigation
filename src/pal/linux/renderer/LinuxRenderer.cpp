#include "pal/linux/renderer/LinuxRenderer.h"

#include <QElapsedTimer>
#include <QFont>
#include <QFontMetrics>
#include <QFuture>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <memory>
#include <utility>

namespace p1::renderer {
namespace {

QColor markerColor(const MapMarker& marker) {
    return marker.color.isValid() ? marker.color : QColor(QStringLiteral("#4335BC"));
}

void drawBase(QImage& image, const MapScene& scene) {
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.fillRect(image.rect(), QColor(QStringLiteral("#EEF2EA")));
    painter.scale(
        image.width() / scene.logicalSize.width(),
        image.height() / scene.logicalSize.height());

    QVector<MapTile> tiles = scene.tiles;
    std::sort(tiles.begin(), tiles.end(), [](const MapTile& left, const MapTile& right) {
        if (left.zOrder != right.zOrder) return left.zOrder < right.zOrder;
        return left.id < right.id;
    });
    for (const auto& tile : tiles) {
        painter.drawImage(tile.target, tile.image);
    }
}

void drawNavigationOverlay(QPainter& painter,
                           const MapScene& scene,
                           const contracts::NavigationSnapshot* navigation) {
    if (!navigation) return;

    const qreal width = scene.logicalSize.width();
    QFont titleFont = painter.font();
    titleFont.setPixelSize(18);
    titleFont.setBold(true);
    QFont bodyFont = painter.font();
    bodyFont.setPixelSize(13);

    if (navigation->guidance) {
        // The top-left map menu occupies 16..72 px. Put guidance below it so
        // both remain readable instead of layering two cards on top of each other.
        const QRectF card(24, 84, qMin<qreal>(360, width - 48), 116);
        painter.setPen(QPen(QColor(67, 53, 188, 70), 1));
        painter.setBrush(QColor(255, 255, 255, 236));
        painter.drawRoundedRect(card, 14, 14);
        painter.setFont(titleFont);
        painter.setPen(QColor(QStringLiteral("#202335")));
        painter.drawText(card.adjusted(18, 12, -18, -64),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         navigation->guidance->text);
        painter.setFont(bodyFont);
        painter.setPen(QColor(QStringLiteral("#625C73")));
        const QString distance = navigation->remainingDistanceM
            ? (*navigation->remainingDistanceM >= 1000.0
                   ? QStringLiteral("剩余 %1 km").arg(*navigation->remainingDistanceM / 1000.0, 0, 'f', 1)
                   : QStringLiteral("剩余 %1 m").arg(*navigation->remainingDistanceM, 0, 'f', 0))
            : QStringLiteral("剩余距离 --");
        const QString duration = navigation->remainingDurationSec
            ? QStringLiteral("预计 %1 min").arg(*navigation->remainingDurationSec / 60.0, 0, 'f', 0)
            : QStringLiteral("预计时间 --");
        painter.drawText(card.adjusted(18, 58, -18, -34),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         distance + QStringLiteral("   ·   ") + duration);
        const QString speed = navigation->currentPosition && navigation->currentPosition->speedMps
            ? QStringLiteral("速度 %1 km/h").arg(*navigation->currentPosition->speedMps * 3.6, 0, 'f', 0)
            : QStringLiteral("速度 --");
        const QString source = navigation->source == contracts::SourceKind::Mock
            ? QStringLiteral("模拟导航")
            : QStringLiteral("实时导航");
        const QString state = navigation->status == contracts::NavigationStatus::Arrived
            ? QStringLiteral("已到达")
            : navigation->paused ? QStringLiteral("已暂停") : source;
        painter.drawText(card.adjusted(18, 82, -18, -10),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         speed + QStringLiteral("   ·   ") + state);
    }
}

void drawOverlays(QPainter& painter,
                  const MapScene& scene,
                  const contracts::NavigationSnapshot* navigation) {
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.scale(
        painter.device()->width() / scene.logicalSize.width(),
        painter.device()->height() / scene.logicalSize.height());

    QVector<RouteLine> routes = scene.routes;
    std::sort(routes.begin(), routes.end(), [](const RouteLine& left, const RouteLine& right) {
        if (left.zOrder != right.zOrder) return left.zOrder < right.zOrder;
        return left.id < right.id;
    });
    for (const auto& route : routes) {
        QPainterPath path(route.points.first());
        for (int index = 1; index < route.points.size(); ++index) {
            path.lineTo(route.points[index]);
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(255, 255, 255, 210), route.width + 3,
                            Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(path);
        painter.setPen(QPen(route.color, route.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(path);
    }

    QVector<MapMarker> markers = scene.markers;
    std::sort(markers.begin(), markers.end(), [](const MapMarker& left, const MapMarker& right) {
        if (left.zOrder != right.zOrder) return left.zOrder < right.zOrder;
        return left.id < right.id;
    });
    QFont markerFont = painter.font();
    markerFont.setPixelSize(12);
    markerFont.setBold(true);
    painter.setFont(markerFont);
    for (const auto& marker : markers) {
        if (marker.kind == MarkerKind::CurrentPosition) {
            painter.setPen(Qt::NoPen);
            QColor halo = markerColor(marker);
            halo.setAlpha(42);
            painter.setBrush(halo);
            painter.drawEllipse(marker.position, marker.radius + 7, marker.radius + 7);

            painter.setPen(QPen(Qt::white, 3));
            painter.setBrush(markerColor(marker));
            painter.drawEllipse(marker.position, marker.radius, marker.radius);

            painter.save();
            painter.translate(marker.position);
            painter.rotate(marker.headingDeg);
            QPainterPath arrow;
            arrow.moveTo(0, -marker.radius + 2);
            arrow.lineTo(marker.radius - 4, marker.radius - 4);
            arrow.lineTo(0, marker.radius - 6);
            arrow.lineTo(-marker.radius + 4, marker.radius - 4);
            arrow.closeSubpath();
            painter.setPen(Qt::NoPen);
            painter.setBrush(Qt::white);
            painter.drawPath(arrow);
            painter.restore();
            continue;
        }
        painter.setPen(QPen(Qt::white, 3));
        painter.setBrush(markerColor(marker));
        painter.drawEllipse(marker.position, marker.radius, marker.radius);
        if (marker.label == "A" || marker.label == "B") {
            painter.setPen(Qt::white);
            painter.drawText(QRectF(marker.position.x()-marker.radius, marker.position.y()-marker.radius,
                                    marker.radius*2, marker.radius*2),Qt::AlignCenter,marker.label);
            continue;
        }
        if (!marker.label.isEmpty()) {
            const QRectF labelRect(marker.position.x() + marker.radius + 6,
                                   marker.position.y() - 26,
                                   160,
                                   22);
            painter.setPen(QColor(QStringLiteral("#202335")));
            painter.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter, marker.label);
        }
    }

    drawNavigationOverlay(painter, scene, navigation);
}

} // namespace

const char* toString(RenderMode mode) {
    return mode == RenderMode::CachedBase ? "cached-base" : "immediate";
}

LinuxRenderer::LinuxRenderer(QObject* parent)
    : QObject(parent),
      watcher_(new QFutureWatcher<RenderResult>(this)) {
    qRegisterMetaType<RenderStats>("p1::renderer::RenderStats");
    pool_.setMaxThreadCount(1);
    connect(watcher_, &QFutureWatcher<RenderResult>::finished,
            this, &LinuxRenderer::finishRequest);
}

LinuxRenderer::~LinuxRenderer() {
    if (watcher_) watcher_->disconnect(this);
    pool_.clear();
    pool_.waitForDone();
}

void LinuxRenderer::setMode(RenderMode mode) {
    mode_ = mode;
}

void LinuxRenderer::requestRender(
    const MapScene& scene,
    const std::optional<contracts::NavigationSnapshot>& navigation,
    const QSize& outputSize) {
    const auto validation = validateScene(scene, outputSize);
    if (!validation.valid) {
        emit errorOccurred(validation.error);
        return;
    }

    Request request{++nextRequestId_, scene, navigation, outputSize};
    latestRequestId_ = request.id;
    if (busy_) {
        pending_ = std::move(request);
        return;
    }
    startRequest(std::move(request));
}

void LinuxRenderer::startRequest(Request request) {
    busy_ = true;
    const QImage cachedBase = cachedBase_;
    const QString cachedKey = cachedBaseKey_;
    const RenderMode requestedMode = mode_;
    watcher_->setFuture(QtConcurrent::run(
        &pool_,
        [scene = std::move(request.scene),
         navigation = request.navigation,
         outputSize = request.outputSize,
         requestedMode,
         requestId = request.id,
         cachedBase,
         cachedKey]() {
            return LinuxRenderer::render(
                scene,
                navigation ? &*navigation : nullptr,
                outputSize,
                requestedMode,
                requestId,
                cachedBase,
                cachedKey);
        }));
}

void LinuxRenderer::finishRequest() {
    RenderResult result;
    try {
        result = watcher_->result();
    } catch (const std::exception& error) {
        emit errorOccurred(QString::fromUtf8(error.what()));
    } catch (...) {
        emit errorOccurred(QStringLiteral("地图渲染发生未知异常"));
    }

    if (result.stats.requestId != 0 &&
        result.stats.requestId == latestRequestId_ &&
        !result.frame.isNull()) {
        if (result.stats.mode == RenderMode::CachedBase) {
            cachedBase_ = result.baseCache;
            cachedBaseKey_ = result.baseCacheKey;
        }
        emit frameReady(result.frame, result.stats);
    }

    busy_ = false;
    if (pending_) {
        Request next = std::move(*pending_);
        pending_.reset();
        startRequest(std::move(next));
    }
}

RenderResult LinuxRenderer::render(
    const MapScene& scene,
    const contracts::NavigationSnapshot* navigation,
    const QSize& outputSize,
    RenderMode mode,
    quint64 requestId,
    const QImage& cachedBase,
    const QString& cachedKey) {
    QElapsedTimer timer;
    timer.start();
    const auto validation = validateScene(scene, outputSize);
    if (!validation.valid) {
        RenderResult invalid;
        invalid.error = validation.error;
        invalid.stats.requestId = requestId;
        invalid.stats.mode = mode;
        invalid.stats.outputSize = outputSize;
        return invalid;
    }

    const QString key = baseCacheKey(scene, outputSize);
    bool cacheHit = false;
    QImage base;
    if (mode == RenderMode::CachedBase &&
        !cachedBase.isNull() &&
        cachedBase.size() == outputSize &&
        cachedKey == key) {
        base = cachedBase;
        cacheHit = true;
    } else {
        base = QImage(outputSize, QImage::Format_ARGB32_Premultiplied);
        base.fill(Qt::transparent);
        drawBase(base, scene);
    }

    QImage frame = base.copy();
    {
        QPainter painter(&frame);
        drawOverlays(painter, scene, navigation);
    }

    RenderResult result;
    result.frame = frame;
    result.baseCache = base;
    result.baseCacheKey = key;
    result.stats.requestId = requestId;
    result.stats.mode = mode;
    result.stats.cacheHit = cacheHit;
    result.stats.tileCount = scene.tiles.size();
    result.stats.routeCount = scene.routes.size();
    result.stats.markerCount = scene.markers.size();
    result.stats.outputSize = outputSize;
    result.stats.elapsedUs = timer.nsecsElapsed() / 1000;
    return result;
}

} // namespace p1::renderer
