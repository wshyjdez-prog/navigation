#pragma once
#include "app/ImportTypes.h"
#include "data/osmx/OsmxDataset.h"
#include "algorithms/model/Types.h"
#include "app/RoutingProfile.h"
#include "algorithms/fmm/FmmClient.h"
#include "contracts/NavigationTypes.h"
#include <QObject>
#include <QPointF>
#include <QThreadPool>
#include <atomic>
#include <functional>
#include <optional>

namespace p1::contracts { class LocationSource; }

namespace p1::app {
// GUI-owned coordinator. All parsing, graph conversion and external processes
// run on workers over immutable snapshots. Geo points use x=longitude,y=latitude.
struct RoadNetwork {
    std::shared_ptr<const data::OsmxDataset> roads;
    algorithms::model::GraphData graph;
    double lat0=0, lon0=0, metresLat=0, metresLon=0;
    QString version;
    QPointF project(QPointF geo) const;
    QPointF unproject(QPointF metric) const;
};
struct WorkflowResult {
    std::shared_ptr<const RoadNetwork> network;
    data::OsmxBounds loadedArea;
    QVector<QPointF> route, observations, matched;
    std::shared_ptr<const std::vector<algorithms::model::MetricPoint>> routeGeometry;
    std::optional<QPointF> destination;
    RouteMode routeMode=RouteMode::General;
    QString message;
    bool ok=false;
    double elapsedMs=0, distanceM=0, algorithmMs=0;
    int count=0;
};

struct LiveNavigationOptions {
    QString fmmExecutable, ubodtExecutable;
    int fixGapMs = 5000;
    int matchTimeoutMs = 10000;
    int rerouteTimeoutMs = 30000;
    std::size_t windowSize = 16;
    // Optional worker instrumentation for timing-controlled integration tests.
    std::function<void()> rerouteResultReady;
    int resourceAuditIntervalMs = 30000;
    int resourceAuditBudgetMs = 30000;
    std::function<void()> resourceAuditStarted;
};

struct LiveNavigationState {
    bool active{}, paused{}, preparing{}, locationLost{}, rerouting{};
    std::uint64_t sessionId{}, routeRevision{};
    QString routeId, networkVersion;
    std::optional<algorithms::FmmStatus> matchStatus;
    std::optional<contracts::MapMatchResult> match;
    std::optional<algorithms::model::EdgeId> matchedEdge;
    std::optional<double> ubodtLoadMs;
    double exportMs{}, ubodtMs{}, matchMs{}, planMs{}, cancellationMs{};
    std::size_t windowPoints{}, preparationCount{};
    quint64 resourceGeneration{};
    qint64 resourceScanMs{};
    QString rerouteError;
};

class NavigationWorkflow final : public QObject {
    Q_OBJECT
public:
    explicit NavigationWorkflow(QObject* parent=nullptr);
    ~NavigationWorkflow() override;
    void load(const ImportResult&, double lat, double lon, double radiusKm);
    void plan(QPointF start, QPointF end);
    bool startNavigation(contracts::LocationSource* source = nullptr,
                         const LiveNavigationOptions& options = {});
    bool startNavigationReplay(const QString& trackFile);
    void locationUpdated(const contracts::PositionSample&);
    void pauseNavigation();
    void resumeNavigation();
    void stopNavigation();
    const LiveNavigationState& liveNavigation() const;
    int navigationTaskCount() const;
    // Applies to the next plan(); matching keeps the full network.
    void setRouteMode(RouteMode mode);
    RouteMode routeMode() const { return routeMode_; }
    // Car-only subgraph (same node/edge ids) used by RouteMode::Driving.
    static algorithms::model::GraphData drivingGraph(const RoadNetwork& network,
        const algorithms::FmmCancellation& cancel = {});
    void match(const QString& csvPath);
    void cancel();
    void beginMapPick(bool start);
    void acceptMapPoint(QPointF geo);
    void setEndpoint(bool start, QPointF geo);
    void clearEndpoints();
    const std::optional<QPointF>& startPoint() const { return startPoint_; }
    const std::optional<QPointF>& endPoint() const { return endPoint_; }
    const std::optional<ImportResult>& activeDataset() const { return activeDataset_; }
    quint64 datasetRevision() const { return datasetRevision_; }
    bool busy() const { return busy_; }
    QString status() const { return status_; }
    QString metrics() const { return metrics_; }
    QString runtimeStatus() const;
    const WorkflowResult& state() const { return state_; }
    const WorkflowResult& lastResult() const { return lastResult_; }
    // Size limits: 250k nodes / 500k edges for regional networks; the tiered
    // long-route network passes higher limits.
    static std::shared_ptr<const RoadNetwork> buildNetwork(std::shared_ptr<const data::OsmxDataset>,
        const std::shared_ptr<std::atomic_bool>& cancel={}, quint32 maxNodes=250000, int maxEdges=500000);
signals:
    void changed();
    void completed(const QString& kind, bool success);
    void mapPickRequested();
    void endpointSelected(bool start, QPointF geo);
    void navigationMatchUpdated();
    void navigationRerouteRequested(const p1::contracts::RerouteRequest& request);
    void navigationRerouteFinished(const p1::contracts::RoutePlanResult& result, const QString& error);
private:
    struct Live;
    void run(const QString& kind, std::function<WorkflowResult(std::shared_ptr<std::atomic_bool>)>);
    QThreadPool pool_;
    QVector<std::shared_ptr<std::atomic_bool>> cancellations_;
    quint64 generation_=0;
    bool busy_=false;
    int pickMode_=0;
    QString status_=QStringLiteral("请先选择已导入的数据并加载工作区域"), metrics_;
    WorkflowResult state_;
    WorkflowResult lastResult_;
    std::optional<ImportResult> activeDataset_, pendingDataset_;
    std::optional<QPointF> startPoint_, endPoint_;
    quint64 datasetRevision_=0;
    RouteMode routeMode_=RouteMode::General;
    std::unique_ptr<Live> live_;
};
}
