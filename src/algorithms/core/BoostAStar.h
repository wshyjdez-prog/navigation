#pragma once

#include "model/Validation.h"
#include <atomic>
#include <memory>

namespace p1::algorithms {

enum class RouteStatus { Success, InvalidGraph, InvalidNode, Unreachable, Cancelled, NumericError, Failed };
using Cancellation = std::shared_ptr<std::atomic_bool>;

// Routing-specific input domain (P0 remains a general finite-value model).
// Applies separately to length_m and cost_m, in metres.
inline constexpr double max_road_value_m = 1e9;
inline constexpr double max_graph_total_m = 1e12;

struct RoutePath {
    RouteStatus status{RouteStatus::Failed};
    std::string message;
    std::string dataset_id;
    std::string data_version;
    std::vector<model::NodeId> nodes;
    std::vector<model::EdgeId> edges;
    std::vector<model::MetricPoint> geometry;
    double total_cost_m{};     // Objective minimized by Boost A*.
    double total_distance_m{}; // Sum of supplied edge.length_m, not cost_m.
    double elapsed_ms{};       // Validation + conversion + search + recovery.
    model::ValidationResult validation;
};

// Synchronous pure core: call from a worker, not the Qt GUI thread.
// Input must remain unchanged during the call. Uses Boost astar_search with
// h=0 because P0 does not establish a geometric lower bound on cost_m.
// Each edge value <= max_road_value_m and each graph-wide sum <=
// max_graph_total_m; unsupported values return NumericError before searching.
RoutePath findRoute(const model::GraphData& graph, model::NodeId origin,
                    model::NodeId destination, const Cancellation& cancellation = {});

} // namespace p1::algorithms
