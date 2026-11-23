#pragma once

#include "algorithms/core/BoostAStar.h"
#include "algorithms/fmm/FmmClient.h"
#include <QPointF>
#include <QTemporaryDir>
#include <stdexcept>

namespace p1::app {

struct RoadNetwork;
enum class RouteMode;

struct MatchingNetwork {
    std::shared_ptr<QTemporaryDir> files;
    algorithms::FmmConfig config;
    double exportMs{};
    double ubodtMs{};
};

class MatchingNetworkError : public std::runtime_error {
public:
    MatchingNetworkError(algorithms::FmmStatus status, const QString& message)
        : std::runtime_error(message.toStdString()), status(status) {}
    const algorithms::FmmStatus status;
};

MatchingNetwork prepareMatchingNetwork(const RoadNetwork&, const algorithms::model::GraphData&,
    const QString& version, const algorithms::Cancellation&, const QString& fmm = {},
    const QString& ubodt = {}, int timeoutMs = 30000);

algorithms::RoutePath planOnSnapshot(const RoadNetwork&, const algorithms::model::GraphData&,
    RouteMode, QPointF start, QPointF end, const algorithms::Cancellation&);

QVector<QPointF> geographicRoute(const RoadNetwork&, const std::vector<algorithms::model::MetricPoint>&);

}
