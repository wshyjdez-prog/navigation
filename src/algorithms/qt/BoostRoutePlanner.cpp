#include "qt/BoostRoutePlanner.h"
#include <QFutureWatcher>
#include <QThread>
#include <QUuid>
#include <QtConcurrentRun>
#include <cmath>

namespace p1::algorithms {
namespace {
bool validCoordinate(const contracts::GeoCoordinate& p) {
    return p.crs == contracts::CoordinateSystem::WGS84 &&
        std::isfinite(p.latitudeDeg) && p.latitudeDeg >= -90 && p.latitudeDeg <= 90 &&
        std::isfinite(p.longitudeDeg) && p.longitudeDeg >= -180 && p.longitudeDeg <= 180 &&
        (!p.altitudeM || std::isfinite(*p.altitudeM));
}

std::optional<model::NodeId> locateNode(const model::GraphData& graph, model::MetricPoint point,
                                      const Cancellation& cancelled) {
    if (!std::isfinite(point.x_m) || !std::isfinite(point.y_m)) return {};
    std::optional<model::NodeId> found;
    for (const auto& node : graph.nodes) {
        if (cancelled->load(std::memory_order_relaxed)) return {};
        if (std::hypot(point.x_m - node.position.x_m, point.y_m - node.position.y_m) <=
            model::endpoint_tolerance_m) {
            if (found) return {}; // Coincident topology nodes are ambiguous.
            found = node.id;
        }
    }
    return found;
}

RoutePath compute(const std::shared_ptr<const model::GraphSnapshot>& snapshot,
                  const CoordinateProjector& projector, const contracts::RoutePlanRequest& request,
                  const Cancellation& cancelled) {
    RoutePath result;
    try {
        if (cancelled->load(std::memory_order_relaxed)) {
            result.status = RouteStatus::Cancelled; return result;
        }
        const auto from = projector(request.origin);
        if (cancelled->load(std::memory_order_relaxed)) {
            result.status = RouteStatus::Cancelled; return result;
        }
        const auto to = projector(request.destination);
        const auto a = from ? locateNode(snapshot->data(), *from, cancelled) : std::nullopt;
        const auto b = to ? locateNode(snapshot->data(), *to, cancelled) : std::nullopt;
        if (cancelled->load(std::memory_order_relaxed)) result.status = RouteStatus::Cancelled;
        else if (!a || !b) {
            result.status = RouteStatus::InvalidNode;
            result.message = "projection failed or endpoint is not a unique existing road node";
        } else result = findRoute(snapshot->data(), *a, *b, cancelled);
    } catch (const std::exception& e) {
        result.status = RouteStatus::Failed; result.message = e.what();
    } catch (...) {
        result.status = RouteStatus::Failed; result.message = "unknown coordinate adapter failure";
    }
    return result;
}
} // namespace

BoostRoutePlanner::BoostRoutePlanner(std::shared_ptr<const model::GraphSnapshot> graph,
                                   CoordinateProjector projector, double assumedSpeedMps, QObject* parent)
    : contracts::RoutePlanner(parent), graph_(std::move(graph)), projector_(std::move(projector)),
      speed_(assumedSpeedMps), instanceId_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    contracts::registerNavigationMetaTypes();
}

BoostRoutePlanner::~BoostRoutePlanner() {
    // Workers own their snapshot/projector/token; they never capture this.
    for (const auto& entry : pending_) entry.second->store(true, std::memory_order_relaxed);
}

std::uint64_t BoostRoutePlanner::requestRoute(const contracts::RoutePlanRequest& request) {
    if (QThread::currentThread() != thread() || !graph_ || !projector_ ||
        !std::isfinite(speed_) || speed_ <= 0 || request.requestId == 0 || request.requestId <= latestId_ ||
        request.dataVersion != QString::fromStdString(graph_->data().metadata.version) ||
        !validCoordinate(request.origin) || !validCoordinate(request.destination) ||
        (request.options && !request.options->isEmpty())) return 0;

    auto* watcher = new QFutureWatcher<RoutePath>(this);
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    // Latest accepted request wins; previously accepted work is cooperatively cancelled.
    for (const auto& entry : pending_) entry.second->store(true, std::memory_order_relaxed);
    latestId_ = request.requestId;
    pending_.emplace(request.requestId, cancellation);
    connect(watcher, &QFutureWatcher<RoutePath>::finished, this,
            [this, watcher, cancellation, id = request.requestId] {
        pending_.erase(id);
        watcher->deleteLater();
        if (cancellation->load(std::memory_order_relaxed) || id != latestId_) return;
        RoutePath path;
        try {
            path = watcher->result(); // Already finished: never waits on the GUI thread.
        } catch (const std::exception& e) {
            path.status = RouteStatus::Failed; path.message = e.what();
        } catch (...) {
            path.status = RouteStatus::Failed; path.message = "unhandled worker exception";
        }
        if (path.status != RouteStatus::Success) {
            emit routeFailed(id, path.status == RouteStatus::Unreachable
                ? contracts::NavigationErrorCode::NoRouteAvailable
                : contracts::NavigationErrorCode::PlanningFailed,
                QString::fromStdString(path.message));
            return;
        }
        const double duration = path.total_distance_m / speed_;
        if (!std::isfinite(duration)) {
            emit routeFailed(id, contracts::NavigationErrorCode::PlanningFailed,
                             QStringLiteral("duration estimate exceeds finite range"));
            return;
        }
        const QString routeId = QStringLiteral("bgl:%1:%2").arg(instanceId_).arg(id);
        const QString newGeometryRef = routeId + QStringLiteral(":metric");
        contracts::RoutePlanRef ref;
        ref.routeId = routeId; ref.requestId = id;
        ref.dataVersion = QString::fromStdString(path.data_version);
        ref.geometryRef = newGeometryRef;
        ref.totalDistanceM = path.total_distance_m;
        ref.totalDurationSec = duration; // Explicit constant-speed estimate, not measured travel time.
        ref.source = contracts::SourceKind::Real; // Real algorithm; input may still be synthetic.
        auto delivered = std::make_shared<const RoutePath>(std::move(path));
        delivered_.emplace(newGeometryRef, delivered);
        lastPath_ = std::move(delivered);
        geometryRef_ = newGeometryRef;
        contracts::RoutePlanResult result;
        result.requestId = id; result.route = std::move(ref);
        emit routeReady(result);
    });
    try {
        watcher->setFuture(QtConcurrent::run([snapshot = graph_, projector = projector_, request, cancellation] {
            return compute(snapshot, projector, request, cancellation);
        }));
    } catch (...) {
        cancellation->store(true, std::memory_order_relaxed);
        pending_.erase(request.requestId);
        watcher->deleteLater();
        return 0;
    }
    return request.requestId;
}

void BoostRoutePlanner::cancel(std::uint64_t requestId) {
    if (QThread::currentThread() != thread()) return;
    const auto entry = pending_.find(requestId);
    if (entry != pending_.end()) entry->second->store(true, std::memory_order_relaxed);
    // Contract: cancellation is silent; no ready/failed for this request afterwards.
}

std::shared_ptr<const RoutePath> BoostRoutePlanner::pathForGeometry(const QString& ref) const {
    const auto found = delivered_.find(ref);
    return found == delivered_.end() ? nullptr : found->second;
}

void BoostRoutePlanner::releaseGeometry(const QString& ref) {
    if (QThread::currentThread() != thread()) return;
    delivered_.erase(ref);
    if (ref == geometryRef_) {
        lastPath_.reset();
        geometryRef_.clear();
    }
}
} // namespace p1::algorithms
