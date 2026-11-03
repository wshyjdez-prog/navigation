#pragma once
#include "contracts/NavigationController.h"
#include "core/BoostAStar.h"
#include <functional>
#include <map>

namespace p1::algorithms {

// Dataset-specific WGS84(degrees) -> this snapshot's metre frame.
// Executed on a worker. Capture owned, thread-safe state; never GUI objects.
using CoordinateProjector = std::function<std::optional<model::MetricPoint>(const contracts::GeoCoordinate&)>;

class BoostRoutePlanner final : public contracts::RoutePlanner {
    Q_OBJECT
public:
    // No implicit projection or speed. Provider must define both explicitly.
    BoostRoutePlanner(std::shared_ptr<const model::GraphSnapshot> graph,
                      CoordinateProjector projector, double assumedSpeedMps,
                      QObject* parent = nullptr);
    ~BoostRoutePlanner() override;
    std::uint64_t requestRoute(const contracts::RoutePlanRequest& request) override;
    void cancel(std::uint64_t requestId) override;

    // Owner-thread access. Delivered routes survive pending/failed/cancelled
    // replans. Consumers release the old ref after accepting its replacement;
    // an owning handle can safely outlive that explicit release.
    const std::shared_ptr<const RoutePath>& lastPath() const noexcept { return lastPath_; }
    QString geometryRef() const { return geometryRef_; }
    std::shared_ptr<const RoutePath> pathForGeometry(const QString& ref) const;
    void releaseGeometry(const QString& ref);

private:
    std::shared_ptr<const model::GraphSnapshot> graph_;
    CoordinateProjector projector_;
    double speed_;
    std::uint64_t latestId_{};
    std::map<std::uint64_t, Cancellation> pending_;
    std::shared_ptr<const RoutePath> lastPath_;
    std::map<QString, std::shared_ptr<const RoutePath>> delivered_;
    QString instanceId_;
    QString geometryRef_;
};
} // namespace p1::algorithms
