#include "ui/map/NavigationSimulation.h"

#include <QDateTime>
#include <QtGlobal>
#include <QtMath>

#include <algorithm>
#include <limits>

namespace p1::ui {
namespace {

constexpr double kMetresPerDegree = 111195.08;

double normalizedDelta(double degrees) {
    while (degrees > 180.0) degrees -= 360.0;
    while (degrees < -180.0) degrees += 360.0;
    return degrees;
}

double segmentDistance(const QPointF& from, const QPointF& to) {
    const double meanLat = qDegreesToRadians((from.y() + to.y()) * 0.5);
    const double dx = (to.x() - from.x()) * kMetresPerDegree * qCos(meanLat);
    const double dy = (to.y() - from.y()) * kMetresPerDegree;
    return qSqrt(dx * dx + dy * dy);
}

double segmentBearing(const QPointF& from, const QPointF& to) {
    const double meanLat = qDegreesToRadians((from.y() + to.y()) * 0.5);
    const double east = (to.x() - from.x()) * qCos(meanLat);
    const double north = to.y() - from.y();
    return qRadiansToDegrees(qAtan2(east, north));
}

QString distanceText(double metres) {
    const int rounded = metres >= 1000.0
        ? int(qRound(metres / 1000.0) * 1000)
        : int(qRound(metres / 10.0) * 10);
    return rounded >= 1000
        ? QStringLiteral("%1 km").arg(rounded / 1000.0, 0, 'f', 1)
        : QStringLiteral("%1 m").arg(rounded);
}

} // namespace

bool NavigationSimulation::setRoute(const QVector<QPointF>& geoRoute,
                                    double speedMps,
                                    const QString& dataVersion) {
    if (geoRoute.size() < 2 || !qIsFinite(speedMps) || speedMps <= 0 ||
        dataVersion.trimmed().isEmpty()) {
        return false;
    }
    for (const auto& point : geoRoute) {
        if (!qIsFinite(point.x()) || !qIsFinite(point.y()) ||
            std::abs(point.x()) > 180.0 || std::abs(point.y()) > 85.0) {
            return false;
        }
    }

    geoRoute_ = geoRoute;
    dataVersion_ = dataVersion;
    baseSpeedMps_ = speedMps;
    cumulativeM_.resize(geoRoute_.size());
    cumulativeM_[0] = 0.0;
    for (int index = 1; index < geoRoute_.size(); ++index) {
        cumulativeM_[index] =
            cumulativeM_[index - 1] + segmentDistance(geoRoute_[index - 1], geoRoute_[index]);
    }
    totalM_ = cumulativeM_.back();
    travelledM_ = 0.0;
    running_ = false;
    paused_ = false;
    arrived_ = false;
    snapshot_.reset();
    return totalM_ > 0.0;
}

void NavigationSimulation::start() {
    if (!hasRoute()) return;
    travelledM_ = 0.0;
    running_ = true;
    paused_ = false;
    arrived_ = false;
    snapshot_ = contracts::NavigationSnapshot{};
    snapshot_->navigationId = ++navigationId_;
    snapshot_->currentRequestId = 1;
    snapshot_->status = contracts::NavigationStatus::Navigating;
    snapshot_->paused = false;
    snapshot_->dataVersion = dataVersion_;
    snapshot_->source = contracts::SourceKind::Mock;
    updateSnapshot();
}

void NavigationSimulation::pause() {
    if (!running_ || arrived_) return;
    paused_ = true;
    updateSnapshot();
}

void NavigationSimulation::resume() {
    if (!running_ || arrived_) return;
    paused_ = false;
    updateSnapshot();
}

void NavigationSimulation::stop() {
    running_ = false;
    paused_ = false;
    snapshot_.reset();
}

void NavigationSimulation::setSpeedMultiplier(double multiplier) {
    if (!qIsFinite(multiplier) || multiplier < 0.25 || multiplier > 16.0) return;
    speedMultiplier_ = multiplier;
}

void NavigationSimulation::advance(int elapsedMs) {
    if (!running_ || paused_ || arrived_ || elapsedMs <= 0 || !hasRoute()) return;
    travelledM_ = qMin(
        totalM_,
        travelledM_ + baseSpeedMps_ * speedMultiplier_ * elapsedMs / 1000.0);
    if (travelledM_ >= totalM_) {
        travelledM_ = totalM_;
        running_ = false;
        arrived_ = true;
    }
    updateSnapshot();
}

void NavigationSimulation::updateSnapshot() {
    if (!snapshot_) snapshot_ = contracts::NavigationSnapshot{};
    double bearing = 0.0;
    const QPointF position = pointAt(travelledM_, &bearing);

    contracts::PositionSample sample;
    sample.coordinate = contracts::GeoCoordinate{
        contracts::CoordinateSystem::WGS84, position.y(), position.x(), std::nullopt};
    sample.timestampMs = QDateTime::currentMSecsSinceEpoch();
    sample.speedMps = (arrived_ || paused_) ? 0.0 : baseSpeedMps_;
    sample.bearingDeg = bearing;
    sample.horizontalAccuracyM = 5.0;
    sample.source = contracts::SourceKind::Mock;
    sample.seq = (snapshot_->currentPosition ? snapshot_->currentPosition->seq + 1 : 0);
    sample.sessionId = snapshot_->navigationId;
    sample.dataVersion = dataVersion_;

    snapshot_->currentPosition = sample;
    snapshot_->status = arrived_
        ? contracts::NavigationStatus::Arrived
        : (paused_ ? contracts::NavigationStatus::Navigating
                   : contracts::NavigationStatus::Navigating);
    snapshot_->paused = paused_;
    snapshot_->remainingDistanceM = remainingM();
    snapshot_->remainingDurationSec = baseSpeedMps_ > 0.0
        ? remainingM() / baseSpeedMps_
        : 0.0;
    snapshot_->dataVersion = dataVersion_;
    snapshot_->source = contracts::SourceKind::Mock;
    snapshot_->updatedAtMs = sample.timestampMs;

    contracts::RoutePlanRef route;
    route.routeId = QStringLiteral("mock-navigation-route");
    route.dataVersion = dataVersion_;
    route.geometryRef = QStringLiteral("ui-navigation-simulation");
    route.totalDistanceM = totalM_;
    route.totalDurationSec = baseSpeedMps_ > 0.0 ? totalM_ / baseSpeedMps_ : 0.0;
    route.source = contracts::SourceKind::Mock;
    route.requestId = snapshot_->currentRequestId;
    snapshot_->route = route;

    updateGuidance();
}

QPointF NavigationSimulation::pointAt(double distanceM, double* bearingDeg) const {
    const double clamped = qBound(0.0, distanceM, totalM_);
    auto upper = std::upper_bound(cumulativeM_.cbegin(), cumulativeM_.cend(), clamped);
    int index = int(upper - cumulativeM_.cbegin());
    index = qBound(1, index, geoRoute_.size() - 1);
    const double startDistance = cumulativeM_[index - 1];
    const double segmentLength = cumulativeM_[index] - startDistance;
    const double fraction = segmentLength > 0.0
        ? (clamped - startDistance) / segmentLength
        : 0.0;
    if (bearingDeg) {
        *bearingDeg = segmentBearing(geoRoute_[index - 1], geoRoute_[index]);
    }
    return geoRoute_[index - 1] +
        (geoRoute_[index] - geoRoute_[index - 1]) * fraction;
}

void NavigationSimulation::updateGuidance() {
    contracts::GuidanceInstruction guidance;
    guidance.index = 0;
    if (arrived_) {
        guidance.kind = contracts::GuidanceKind::ArriveDestination;
        guidance.text = QStringLiteral("已到达目的地");
        guidance.distanceToManeuverM = 0.0;
        guidance.index = std::uint32_t(geoRoute_.size() + 1);
        snapshot_->status = contracts::NavigationStatus::Arrived;
    } else {
        int turnIndex = -1;
        double turnDelta = 0.0;
        for (int index = 1; index + 1 < geoRoute_.size(); ++index) {
            if (cumulativeM_[index] <= travelledM_ + 5.0) continue;
            const double incoming = segmentBearing(geoRoute_[index - 1], geoRoute_[index]);
            const double outgoing = segmentBearing(geoRoute_[index], geoRoute_[index + 1]);
            const double delta = normalizedDelta(outgoing - incoming);
            if (std::abs(delta) >= 18.0) {
                turnIndex = index;
                turnDelta = delta;
                break;
            }
        }
        if (turnIndex >= 0) {
            guidance.kind = turnDelta >= 0.0
                ? contracts::GuidanceKind::TurnRight
                : contracts::GuidanceKind::TurnLeft;
            guidance.distanceToManeuverM = cumulativeM_[turnIndex] - travelledM_;
            guidance.index = std::uint32_t(turnIndex);
            guidance.text = QStringLiteral("前方 %1%2")
                .arg(distanceText(guidance.distanceToManeuverM))
                .arg(turnDelta >= 0.0 ? QStringLiteral("右转") : QStringLiteral("左转"));
        } else {
            guidance.kind = contracts::GuidanceKind::ContinueStraight;
            guidance.distanceToManeuverM = remainingM();
            guidance.index = std::uint32_t(geoRoute_.size());
            guidance.text = remainingM() > 20.0
                ? QStringLiteral("沿当前道路直行")
                : QStringLiteral("即将到达目的地");
        }
    }
    snapshot_->guidance = guidance;
    snapshot_->guidanceIndex = guidance.index;
}

} // namespace p1::ui
