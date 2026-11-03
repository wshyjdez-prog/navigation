#include "qt/FmmMapMatcher.h"
#include <QFutureWatcher>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
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
};
} // namespace

FmmMapMatcher::FmmMapMatcher(const FmmConfig& config, FmmCoordinateTransform transform,
                             std::size_t windowSize, QObject* parent)
    : MapMatcher(parent), config_(std::make_shared<const FmmConfig>(config)), transform_(std::move(transform)),
      windowSize_(windowSize) {
    // Deep-copy the ID table once. Existing mutable references into the
    // caller's map cannot modify this published config; tasks share it read-only.
    contracts::registerNavigationMetaTypes();
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
            deliver(request, waiting, {}, flag);
        });
        return request.requestId;
    }
    const auto config = config_;
    const auto transform = transform_;
    const auto positions = window_; // immutable copies, no capture of `this` in worker
    auto* watcher = new QFutureWatcher<WorkResult>(this);
    connect(watcher, &QFutureWatcher<WorkResult>::finished, this,
            [this, watcher, request, flag] {
        const auto work = watcher->result();
        watcher->deleteLater();
        deliver(request, work.batch, work.matched, flag);
    });
    watcher->setFuture(QtConcurrent::run([config, transform, positions, flag] {
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
            work.batch = runFmm(*config, points, flag);
            if (work.batch.status == FmmStatus::Matched) {
                work.matched = transform.toWgs84(work.batch.projectedPoints.back());
                if (!work.matched || !validWgs84(*work.matched)) {
                    work.matched.reset();
                    work.batch.status = FmmStatus::InvalidOutput;
                    work.batch.message = QStringLiteral("Network to WGS84 transform failed");
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
        const std::optional<contracts::GeoCoordinate>& matched, const FmmCancellation& flag) {
    pending_.erase(request.requestId);
    if (flag->load() || request.requestId != latestId_) return;
    lastDetails_ = std::make_shared<const FmmMatchDetails>(
        FmmMatchDetails{request.requestId, request.locationSeq, std::move(batch)});
    contracts::MapMatchResult output;
    output.requestId = request.requestId;
    output.locationSeq = request.locationSeq;
    if (lastDetails_->batch.status == FmmStatus::Matched) output.matched = matched;
    // No reference route geometry is supplied. Road error/offset MUST NOT be
    // mislabelled distanceToRouteM / alongRouteDistanceM. Both remain empty.
    const auto delivered = lastDetails_;
    QPointer<FmmMapMatcher> guard(this);
    emit matchReady(output);
    if (guard && !flag->load() && latestId_ == request.requestId && lastDetails_ == delivered)
        emit detailsReady(request.requestId);
}
} // namespace p1::algorithms
