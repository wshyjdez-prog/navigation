#include "qt/FmmMapMatcher.h"
#include <QFutureWatcher>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace p1::algorithms {
namespace {
bool validWgs84(const contracts::GeoCoordinate& p) {
    return p.crs == contracts::CoordinateSystem::WGS84 &&
        std::isfinite(p.longitudeDeg) && std::isfinite(p.latitudeDeg) &&
        p.longitudeDeg >= -180 && p.longitudeDeg <= 180 &&
        p.latitudeDeg >= -90 && p.latitudeDeg <= 90;
}
struct WorkResult {
    FmmResult batch;
    std::optional<contracts::GeoCoordinate> matched;
    std::optional<RouteProgress> progress;
};
} // namespace

std::optional<RouteProgress> measureRouteProgress(
    const std::vector<model::MetricPoint>& route, FmmPoint point) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) return {};
    std::optional<RouteProgress> best;
    double prefix = 0;
    for (std::size_t i = 1; i < route.size(); ++i) {
        const auto& a = route[i - 1];
        const auto& b = route[i];
        const double dx = b.x_m - a.x_m, dy = b.y_m - a.y_m;
        const double length = std::hypot(dx, dy);
        if (!std::isfinite(length)) return {};
        if (length == 0) continue;
        const double ux = dx / length, uy = dy / length;
        const double px = point.x - a.x_m, py = point.y - a.y_m;
        const double offset = std::clamp(px * ux + py * uy, 0.0, length);
        const double distance = std::hypot(px - ux * offset, py - uy * offset);
        if (!std::isfinite(distance) || !std::isfinite(prefix + length)) return {};
        if (!best || distance < best->distanceM) best = RouteProgress{distance, prefix + offset};
        prefix += length;
    }
    return best;
}

FmmMapMatcher::FmmMapMatcher(FmmConfig config, FmmCoordinateTransform transform,
                             std::size_t windowSize, QObject* parent)
    : MapMatcher(parent), config_(std::make_shared<const FmmConfig>(std::move(config))), transform_(std::move(transform)),
      windowSize_(windowSize) {
    // Deep-copy the ID table once. Existing mutable references into the
    // caller's map cannot modify this published config; tasks share it read-only.
    contracts::registerNavigationMetaTypes();
    if (config_->resources) verified_.start();
}
FmmMapMatcher::~FmmMapMatcher() {
    for (auto& entry : pending_) entry.second->store(true);
    // Watchers disconnect with QObject destruction. Workers own all captures
    // and reap their subprocess before deleting their temporary directory.
}
void FmmMapMatcher::cancel(std::uint64_t id) {
    if (QThread::currentThread() != thread()) return;
    const auto found = pending_.find(id);
    if (found != pending_.end()) {
        found->second->store(true);
        pending_.erase(found);
    }
}
void FmmMapMatcher::resetWindow() {
    if (QThread::currentThread() != thread()) return;
    for (auto& entry : pending_) entry.second->store(true);
    pending_.clear();
    window_.clear();
    routeId_.reset();
    lastSeq_.reset();
    lastDetails_.reset();
    // requestId remains monotonic across sessions to exclude late results.
}

void FmmMapMatcher::setReferenceRoute(
        QString routeId, std::shared_ptr<const std::vector<model::MetricPoint>> geometry) {
    if (QThread::currentThread() != thread()) return;
    resetWindow();
    referenceId_ = std::move(routeId);
    referenceGeometry_ = std::move(geometry);
}

std::uint64_t FmmMapMatcher::requestMatch(const contracts::MapMatchRequest& request) {
    if (QThread::currentThread() != thread() || request.requestId == 0 ||
        request.requestId <= latestId_ || (lastSeq_ && request.locationSeq <= *lastSeq_) ||
        !validWgs84(request.position) || windowSize_ < 2 || windowSize_ > 256 ||
        !transform_.toNetwork || !transform_.toWgs84) return 0;
    for (auto& entry : pending_) entry.second->store(true);
    pending_.clear();
    latestId_ = request.requestId;
    lastSeq_ = request.locationSeq;
    if (request.routeId != routeId_) window_.clear();
    routeId_ = request.routeId;
    window_.push_back(request.position); // duplicate coordinates retained, seq is order
    if (window_.size() > windowSize_) window_.erase(window_.begin());
    const auto flag = std::make_shared<std::atomic_bool>(false);
    pending_[request.requestId] = flag;
    if (window_.size() < 2) {
        FmmResult waiting;
        waiting.status = FmmStatus::WaitingForPoints;
        waiting.message = QStringLiteral("HMM needs at least two ordered observations");
        waiting.datasetId = config_->datasetId;
        waiting.dataVersion = config_->networkVersion;
        waiting.synthetic = config_->units == FmmUnits::SyntheticSampleUnits;
        waiting.inputPointCount = window_.size();
        QTimer::singleShot(0, this, [this, request, waiting, flag] {
            deliver(request, waiting, {}, {}, flag);
        });
        return request.requestId;
    }
    const auto config = config_;
    const FmmAuditRequest audit{knownTimes_,
        (verified_.isValid() && verified_.elapsed() >= config->auditIntervalMs) ||
            backendTimeouts_ >= 2 || failedScans_ != 0,
        !scanSettled_.isValid() || scanSettled_.elapsed() >= 5000, failedScans_ >= 2};
    const auto transform = transform_;
    const auto positions = window_; // immutable copies, no capture of `this` in worker
    const auto reference = request.routeId == referenceId_ ? referenceGeometry_ : nullptr;
    auto* watcher = new QFutureWatcher<WorkResult>(this);
    connect(watcher, &QFutureWatcher<WorkResult>::finished, this,
            [this, watcher, request, flag] {
        auto work = watcher->future().takeResult();
        watcher->deleteLater();
        deliver(request, std::move(work.batch), work.matched, work.progress, flag);
    });
    watcher->setFuture(QtConcurrent::run([config, transform, positions, reference, flag, audit] {
        WorkResult work;
        work.batch.datasetId = config->datasetId;
        work.batch.dataVersion = config->networkVersion;
        work.batch.synthetic = config->units == FmmUnits::SyntheticSampleUnits;
        work.batch.inputPointCount = positions.size();
        try {
            std::vector<FmmPoint> points;
            for (const auto& position : positions) {
                if (flag->load()) { work.batch.status = FmmStatus::Cancelled; return work; }
                const auto point = transform.toNetwork(position);
                if (!point || !std::isfinite(point->x) || !std::isfinite(point->y)) {
                    work.batch.status = FmmStatus::InvalidInput;
                    work.batch.message = QStringLiteral("WGS84 to network transform rejected input");
                    return work;
                }
                points.push_back(*point);
            }
            work.batch = runFmm(*config, points, flag, audit);
            if (work.batch.status == FmmStatus::Matched) {
                work.matched = transform.toWgs84(work.batch.projectedPoints.back());
                if (!work.matched || !validWgs84(*work.matched)) {
                    work.matched.reset();
                    work.batch.status = FmmStatus::InvalidOutput;
                    work.batch.message = QStringLiteral("Network to WGS84 transform failed");
                } else if (reference && !flag->load()) {
                    work.progress = measureRouteProgress(*reference, work.batch.projectedPoints.back());
                }
            }
        } catch (const std::exception& e) {
            work.batch.status = FmmStatus::ExecutionFailed;
            work.batch.message = QString::fromUtf8(e.what());
            work.matched.reset();
        } catch (...) {
            work.batch.status = FmmStatus::ExecutionFailed;
            work.batch.message = QStringLiteral("FMM adapter exception");
            work.matched.reset();
        }
        return work;
    }));
    return request.requestId;
}

void FmmMapMatcher::deliver(const contracts::MapMatchRequest& request, FmmResult batch,
        const std::optional<contracts::GeoCoordinate>& matched,
        const std::optional<RouteProgress>& progress, const FmmCancellation& flag) {
    pending_.erase(request.requestId);
    QPointer<FmmMapMatcher> guard(this);
    if (config_->resources) {
        using Health = FmmResourceReport::Health;
        const auto& report = batch.resource;
        if (report.scanned) scanSettled_.start();
        if (report.health == Health::Verified) {
            verified_.start();
            knownTimes_ = report.times;
            backendTimeouts_ = failedScans_ = 0;
        }
        if (report.health == Health::ScanFailed && ++failedScans_ >= 2)
            batch.message += QStringLiteral(" · 资源核验预算已用完，请停止并重新开始导航");
        if (report.backendTimeout && backendTimeouts_ < 2) ++backendTimeouts_;
        else if (!report.backendTimeout && (report.health == Health::Verified ||
                 (report.health == Health::Unchecked && batch.status != FmmStatus::Cancelled)))
            backendTimeouts_ = 0;
        if (report.health != Health::Unchecked) emit resourceChecked(request.requestId, report);
    }
    if (!guard) return;
    if (flag->load() || request.requestId != latestId_ ||
        batch.resource.health == FmmResourceReport::Health::Corrupt) {
        emit settled(request.requestId);
        return;
    }
    lastDetails_ = std::make_shared<const FmmMatchDetails>(
        FmmMatchDetails{request.requestId, request.locationSeq, std::move(batch)});
    contracts::MapMatchResult output;
    output.requestId = request.requestId;
    output.locationSeq = request.locationSeq;
    if (lastDetails_->batch.status == FmmStatus::Matched) output.matched = matched;
    if (output.matched && progress) {
        output.distanceToRouteM = progress->distanceM;
        output.alongRouteDistanceM = progress->alongM;
    }
    const auto delivered = lastDetails_;
    emit matchReady(output);
    if (guard && !flag->load() && latestId_ == request.requestId && lastDetails_ == delivered)
        emit detailsReady(request.requestId);
    if (guard) emit settled(request.requestId);
}
} // namespace p1::algorithms
