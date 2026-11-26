#pragma once
#include "app/ImportTypes.h"
#include "data/osmx/OsmxDataset.h"
#include "algorithms/model/Types.h"
#include "algorithms/roadmodel/RoadModel.h"
#include "app/RoutingProfile.h"
#include <QObject>
#include <QPointF>
#include <QThreadPool>
#include <atomic>
#include <functional>
#include <optional>

namespace p1::app {
// GUI-owned coordinator. All parsing, graph conversion and external processes
// run on workers over immutable snapshots. Geo points use x=longitude,y=latitude.
// The routable model (projection, directed graph, snapping index, car
// profile) is built once per network by the open-source road model
// (DEC-20261126-1-road-model-01); node index = OSMX node index.
struct RoadNetwork {
    std::shared_ptr<const data::OsmxDataset> roads;
    std::shared_ptr<const algorithms::roadmodel::RoadModel> model;
    QString version;
    const algorithms::model::GraphData& graph() const { return model->graph(); }
    QPointF project(QPointF geo) const;
    QPointF unproject(QPointF metric) const;
};
struct WorkflowResult {
    std::shared_ptr<const RoadNetwork> network;
    data::OsmxBounds loadedArea;
    QVector<QPointF> route, observations, matched;
    QString message;
    bool ok=false;
    double elapsedMs=0, distanceM=0, algorithmMs=0;
    int count=0;
};
class NavigationWorkflow final : public QObject {
    Q_OBJECT
public:
    explicit NavigationWorkflow(QObject* parent=nullptr);
    ~NavigationWorkflow() override;
    void load(const ImportResult&, double lat, double lon, double radiusKm);
    void plan(QPointF start, QPointF end);
    // Applies to the next plan(); matching keeps the full network.
    void setRouteMode(RouteMode mode);
    RouteMode routeMode() const { return routeMode_; }
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
private:
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
};
}
