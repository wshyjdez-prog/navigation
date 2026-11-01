#include "core/BoostAStar.h"
#include "samples/FixedRoadNetwork.h"
#include "TestSupport.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace p1::algorithms;
using namespace p1::algorithms::model;

namespace {
using test_support::require;
void emptyFailure(const RoutePath& p, RouteStatus status) {
    require(p.status == status, "wrong failure category");
    require(p.nodes.empty() && p.edges.empty() && p.geometry.empty(), "partial failed route leaked");
    require(!p.message.empty(), "missing failure reason");
}
}

int main() {
    test_support::Runner test;
    test("normal shortest route and parallel edge recovery", [] {
        const auto p = findRoute(samples::fixedRoadNetwork(), NodeId{900}, NodeId{5});
        require(p.status == RouteStatus::Success, "route not found");
        require(p.nodes == std::vector<NodeId>{NodeId{900}, NodeId{42}, NodeId{5}}, "wrong nodes");
        require(p.edges == std::vector<EdgeId>{EdgeId{10}, EdgeId{11}}, "wrong parallel edge");
        require(p.total_cost_m == 6 && p.total_distance_m == 6, "wrong totals");
        require(p.geometry.front().x_m == 0 && p.geometry.back().x_m == 6, "wrong geometry direction");
        require(p.data_version == "v1" && p.dataset_id == "member3-astar-synthetic", "version lost");
        require(std::isfinite(p.elapsed_ms) && p.elapsed_ms >= 0, "invalid elapsed time");
    });
    test("empty graph rejected with P0 location", [] {
        auto g = samples::fixedRoadNetwork(); g.nodes.clear(); g.edges.clear();
        const auto p = findRoute(g, NodeId{900}, NodeId{5});
        emptyFailure(p, RouteStatus::InvalidGraph);
        require(p.validation.errors.size() == 1 && p.validation.errors[0].code == ErrorCode::EmptyGraph &&
                p.validation.errors[0].object == ObjectKind::Graph && !p.validation.errors[0].index,
                "wrong empty graph diagnosis");
    });
    test("invalid origin and destination IDs", [] {
        const auto g = samples::fixedRoadNetwork();
        emptyFailure(findRoute(g, NodeId{123}, NodeId{5}), RouteStatus::InvalidNode);
        emptyFailure(findRoute(g, NodeId{900}, NodeId{123}), RouteStatus::InvalidNode);
    });
    test("zero heuristic respects zero cost and independent length", [] {
        auto g = samples::fixedRoadNetwork(); g.edges[2].cost_m = 0; g.edges[3].cost_m = 0;
        const auto p = findRoute(g, NodeId{900}, NodeId{5});
        require(p.status == RouteStatus::Success && p.total_cost_m == 0 && p.total_distance_m == 15,
                "cost optimization confused with geometry or length");
        require(p.edges == std::vector<EdgeId>{EdgeId{12}, EdgeId{13}}, "wrong zero-cost path");
    });
    test("pre-cancelled request", [] {
        auto flag = std::make_shared<std::atomic_bool>(true);
        emptyFailure(findRoute(samples::fixedRoadNetwork(), NodeId{900}, NodeId{5}, flag), RouteStatus::Cancelled);
    });
    test("invalid graph weight and coordinate", [] {
        auto g = samples::fixedRoadNetwork(); g.edges[0].cost_m = -1;
        emptyFailure(findRoute(g, NodeId{900}, NodeId{5}), RouteStatus::InvalidGraph);
        g = samples::fixedRoadNetwork(); g.nodes[0].position.x_m = std::numeric_limits<double>::quiet_NaN();
        emptyFailure(findRoute(g, NodeId{900}, NodeId{5}), RouteStatus::InvalidGraph);
    });
    test("routing numeric bounds are enforced before search", [] {
        auto g = samples::fixedRoadNetwork();
        // F2 review counterexample: overflow branch plus finite direct target.
        // P0 accepts these values; routing explicitly rejects this input domain.
        g.edges.resize(3);
        g.edges[0].cost_m = 1e308; // 900 -> 42
        g.edges[1].from = NodeId{900}; g.edges[1].to = NodeId{5};
        g.edges[1].geometry = {{0, 0}, {6, 0}}; g.edges[1].cost_m = 1.5e308;
        g.edges[2].from = NodeId{42}; g.edges[2].to = NodeId{701};
        g.edges[2].geometry = {{3, 0}, {0, 5}}; g.edges[2].cost_m = 1e308;
        require(validateGraph(g).ok(), "counterexample must remain P0-valid");
        const auto unsupported = findRoute(g, NodeId{900}, NodeId{5});
        emptyFailure(unsupported, RouteStatus::NumericError);
        require(unsupported.message.find("bounds exceeded") != std::string::npos, "missing input bound diagnosis");
        g.edges[0].cost_m = 4e8; g.edges[1].cost_m = 6e8; g.edges[2].cost_m = 7e8;
        const auto finite = findRoute(g, NodeId{900}, NodeId{5});
        require(finite.status == RouteStatus::Success && finite.total_cost_m == 6e8 &&
                finite.edges == std::vector<EdgeId>{EdgeId{11}},
                "valid finite target route lost to an expensive branch");

        g = samples::fixedRoadNetwork();
        g.edges[0].length_m = max_road_value_m + 1;
        emptyFailure(findRoute(g, NodeId{900}, NodeId{5}), RouteStatus::NumericError);
        g = samples::fixedRoadNetwork();
        g.edges[0].cost_m = max_road_value_m + 1;
        emptyFailure(findRoute(g, NodeId{900}, NodeId{5}), RouteStatus::NumericError);

        // 1000 parallel edges reach both inclusive graph-wide boundaries.
        const auto edge = samples::fixedRoadNetwork().edges[0];
        g.edges.assign(1000, edge);
        for (std::size_t i = 0; i < g.edges.size(); ++i) {
            g.edges[i].id = EdgeId{i};
            g.edges[i].cost_m = g.edges[i].length_m = max_road_value_m;
        }
        const auto boundary = findRoute(g, NodeId{900}, NodeId{42});
        require(boundary.status == RouteStatus::Success && boundary.total_cost_m == max_road_value_m &&
                boundary.total_distance_m == max_road_value_m, "inclusive routing bound rejected");
        auto extra = edge; extra.id = EdgeId{1000}; extra.cost_m = 0; extra.length_m = 1;
        g.edges.push_back(extra);
        emptyFailure(findRoute(g, NodeId{900}, NodeId{42}), RouteStatus::NumericError);
        // Isolate the total-cost bound from the total-length bound.
        for (auto& e : g.edges) e.length_m = 1;
        g.edges.back().cost_m = 1;
        emptyFailure(findRoute(g, NodeId{900}, NodeId{42}), RouteStatus::NumericError);
    });
    test("all pairs against independent Floyd-Warshall oracle", [] {
        const auto g = samples::fixedRoadNetwork();
        const auto n = g.nodes.size();
        const double inf = std::numeric_limits<double>::infinity();
        std::vector<std::vector<double>> distance(n, std::vector<double>(n, inf));
        const auto nodeIndex = [&](NodeId id) {
            for (std::size_t i = 0; i < n; ++i) if (g.nodes[i].id == id) return i;
            throw std::logic_error("fixture node missing");
        };
        for (std::size_t i = 0; i < n; ++i) distance[i][i] = 0;
        for (const auto& e : g.edges) {
            auto& d = distance[nodeIndex(e.from)][nodeIndex(e.to)]; d = std::min(d, e.cost_m);
        }
        for (std::size_t k = 0; k < n; ++k)
            for (std::size_t i = 0; i < n; ++i)
                for (std::size_t j = 0; j < n; ++j)
                    distance[i][j] = std::min(distance[i][j], distance[i][k] + distance[k][j]);
        for (std::size_t i = 0; i < n; ++i) for (std::size_t j = 0; j < n; ++j) {
            const auto p = findRoute(g, g.nodes[i].id, g.nodes[j].id);
            if (std::isinf(distance[i][j])) emptyFailure(p, RouteStatus::Unreachable);
            else {
                require(p.status == RouteStatus::Success && p.total_cost_m == distance[i][j], "oracle cost mismatch");
                if (i == j) require(p.nodes == std::vector<NodeId>{g.nodes[i].id} && p.edges.empty() &&
                                    p.geometry.size() == 1 && p.total_distance_m == 0,
                                    "same-node route shape mismatch");
            }
        }
    });
    return test.finish();
}
