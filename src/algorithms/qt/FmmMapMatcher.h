#pragma once
#include "contracts/NavigationController.h"
#include "fmm/FmmClient.h"
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

class FmmMapMatcher final : public contracts::MapMatcher {
    Q_OBJECT
public:
    FmmMapMatcher(const FmmConfig& config, FmmCoordinateTransform transform,
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
    std::shared_ptr<const FmmMatchDetails> lastDetails() const { return lastDetails_; }
signals:
    void detailsReady(quint64 requestId); // owner thread; inspect/copy lastDetails()
private:
    void deliver(const contracts::MapMatchRequest&, FmmResult,
                 const std::optional<contracts::GeoCoordinate>&, const FmmCancellation&);
    std::shared_ptr<const FmmConfig> config_;
    FmmCoordinateTransform transform_;
    std::size_t windowSize_;
    std::vector<contracts::GeoCoordinate> window_;
    std::optional<QString> routeId_;
    std::uint64_t latestId_{};
    std::optional<std::uint64_t> lastSeq_;
    std::map<std::uint64_t, FmmCancellation> pending_;
    std::shared_ptr<const FmmMatchDetails> lastDetails_;
};
} // namespace p1::algorithms
