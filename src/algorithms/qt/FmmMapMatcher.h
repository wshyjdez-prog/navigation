#pragma once
#include "contracts/NavigationController.h"
#include "fmm/FmmClient.h"
#include <QElapsedTimer>
#include <functional>
#include <optional>

namespace p1::algorithms {
// Explicit dataset transforms, run on workers; capture owned thread-safe state.
// Production: WGS84 degrees <-> projected network METRES. No implicit identity.
struct FmmCoordinateTransform {
    std::function<std::optional<FmmPoint>(const contracts::GeoCoordinate&)> toNetwork;
    std::function<std::optional<contracts::GeoCoordinate>(FmmPoint)> toWgs84;
};
struct FmmMatchDetails {
    std::uint64_t requestId{}, locationSeq{};
    FmmResult batch;
};

struct RouteProgress {
    double distanceM{};
    double alongM{};
};

std::optional<RouteProgress> measureRouteProgress(
    const std::vector<model::MetricPoint>& route, FmmPoint point);

class FmmMapMatcher final : public contracts::MapMatcher {
    Q_OBJECT
public:
    FmmMapMatcher(FmmConfig config, FmmCoordinateTransform transform,
                  std::size_t windowSize = 16, QObject* parent = nullptr);
    ~FmmMapMatcher() override;
    // Owner thread only. requestId must be positive and strictly increasing.
    // locationSeq may start at 0; subsequent samples must strictly increase.
    // Invalid requests return 0 without changing current work.
    std::uint64_t requestMatch(const contracts::MapMatchRequest&) override;
    // Concrete extensions; public MapMatcher remains unchanged. Cancellation
    // is silent. resetWindow() is required on stop/new location session/gap.
    void cancel(std::uint64_t requestId);
    void resetWindow();
    void setReferenceRoute(QString routeId,
                           std::shared_ptr<const std::vector<model::MetricPoint>> geometry);
    std::shared_ptr<const FmmMatchDetails> lastDetails() const { return lastDetails_; }
signals:
    void detailsReady(quint64 requestId); // owner thread; inspect/copy lastDetails()
    void resourceChecked(quint64 requestId, const FmmResourceReport& report);
    // Also emitted after a cancelled task has reaped its subprocess.
    void settled(quint64 requestId);
private:
    void deliver(const contracts::MapMatchRequest&, FmmResult,
                 const std::optional<contracts::GeoCoordinate>&, const std::optional<RouteProgress>&,
                 const FmmCancellation&);
    std::shared_ptr<const FmmConfig> config_;
    FmmCoordinateTransform transform_;
    std::size_t windowSize_;
    std::vector<contracts::GeoCoordinate> window_;
    std::optional<QString> routeId_;
    QString referenceId_;
    std::shared_ptr<const std::vector<model::MetricPoint>> referenceGeometry_;
    std::uint64_t latestId_{};
    std::optional<std::uint64_t> lastSeq_;
    std::map<std::uint64_t, FmmCancellation> pending_;
    std::shared_ptr<const FmmMatchDetails> lastDetails_;
    QElapsedTimer verified_, scanSettled_;
    std::optional<FmmResourceTimes> knownTimes_;
    unsigned failedScans_{}, backendTimeouts_{};
};
} // namespace p1::algorithms
