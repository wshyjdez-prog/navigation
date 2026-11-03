#pragma once
#include "app/ImportTypes.h"
#include "data/osmx/OsmxDataset.h"
#include "algorithms/model/Types.h"
#include <QObject>
#include <QPointF>
#include <QThreadPool>
#include <atomic>
#include <functional>

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
    void match(const QString& csvPath);
    void cancel();
    void beginMapPick(bool start);
    void acceptMapPoint(QPointF geo);
    bool busy() const { return busy_; }
    QString status() const { return status_; }
    QString metrics() const { return metrics_; }
    QString runtimeStatus() const;
    const WorkflowResult& state() const { return state_; }
    const WorkflowResult& lastResult() const { return lastResult_; }
    static std::shared_ptr<const RoadNetwork> buildNetwork(std::shared_ptr<const data::OsmxDataset>,
        const std::shared_ptr<std::atomic_bool>& cancel={});
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
};
}
