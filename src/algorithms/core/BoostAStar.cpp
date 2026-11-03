#include "core/BoostAStar.h"

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/astar_search.hpp>
#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace p1::algorithms {
namespace {
struct EdgeData { std::size_t input_index; double cost; };
using Graph = boost::adjacency_list<boost::vecS, boost::vecS, boost::directedS,
                                   boost::no_property, EdgeData>;
using Vertex = boost::graph_traits<Graph>::vertex_descriptor;
struct GoalReached {};
struct SearchCancelled {};

void checkCancellation(const Cancellation& cancellation) {
    if (cancellation && cancellation->load(std::memory_order_relaxed)) throw SearchCancelled{};
}

struct Visitor : boost::default_astar_visitor {
    Vertex goal;
    std::vector<std::optional<std::size_t>>& incoming;
    Cancellation cancellation;
    Visitor(Vertex g, std::vector<std::optional<std::size_t>>& p, Cancellation c)
        : goal(g), incoming(p), cancellation(std::move(c)) {}
    void examine_vertex(Vertex v, const Graph&) const {
        checkCancellation(cancellation);
        if (v == goal) throw GoalReached{};
    }
    void examine_edge(Graph::edge_descriptor, const Graph&) const { checkCancellation(cancellation); }
    void edge_relaxed(Graph::edge_descriptor e, const Graph& graph) const {
        incoming[boost::target(e, graph)] = graph[e].input_index;
    }
};
} // namespace

RoutePath findRoute(const model::GraphData& input, model::NodeId origin,
                    model::NodeId destination, const Cancellation& cancellation) {
    const auto started = std::chrono::steady_clock::now();
    RoutePath result;
    result.dataset_id = input.metadata.dataset_id;
    result.data_version = input.metadata.version;
    const auto finish = [&]() {
        result.elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        return std::move(result);
    };
    try {
        checkCancellation(cancellation);
        result.validation = model::validateGraph(input);
        checkCancellation(cancellation);
        if (!result.validation.ok()) {
            result.status = RouteStatus::InvalidGraph;
            result.message = "P0 graph validation failed";
            return finish();
        }
        // A deliberately bounded road-data domain makes ordinary double sums
        // sufficient. Any accepted simple path is bounded by these graph sums;
        // even a candidate extension stays far below double overflow.
        double total_cost = 0;
        double total_length = 0;
        for (std::size_t i = 0; i < input.edges.size(); ++i) {
            checkCancellation(cancellation);
            const auto& edge = input.edges[i];
            if (edge.cost_m > max_road_value_m || edge.length_m > max_road_value_m ||
                edge.cost_m > max_graph_total_m - total_cost ||
                edge.length_m > max_graph_total_m - total_length) {
                result.status = RouteStatus::NumericError;
                result.message = "routing bounds exceeded at edge index " + std::to_string(i) +
                    ": each length/cost <= 1e9 m; each graph-wide sum <= 1e12 m";
                return finish();
            }
            total_cost += edge.cost_m;
            total_length += edge.length_m;
        }
        Graph graph(input.nodes.size());
        std::unordered_map<std::uint64_t, Vertex> vertices;
        vertices.reserve(input.nodes.size());
        for (std::size_t i = 0; i < input.nodes.size(); ++i) {
            checkCancellation(cancellation);
            vertices.emplace(input.nodes[i].id.value, i);
        }
        const auto from = vertices.find(origin.value);
        const auto to = vertices.find(destination.value);
        if (from == vertices.end() || to == vertices.end()) {
            result.status = RouteStatus::InvalidNode;
            result.message = "origin or destination node ID does not exist";
            return finish();
        }
        for (std::size_t i = 0; i < input.edges.size(); ++i) {
            checkCancellation(cancellation);
            const auto& edge = input.edges[i];
            boost::add_edge(vertices.at(edge.from.value), vertices.at(edge.to.value),
                            EdgeData{i, edge.cost_m}, graph);
        }
        std::vector<double> distance(input.nodes.size());
        std::vector<std::optional<std::size_t>> incoming(input.nodes.size());
        const auto index = boost::get(boost::vertex_index, graph);
        bool reached = false;
        try {
            boost::astar_search(graph, from->second,
                boost::astar_heuristic<Graph, double>{},
                boost::weight_map(boost::get(&EdgeData::cost, graph))
                    .distance_map(boost::make_iterator_property_map(distance.begin(), index))
                    .distance_inf(std::numeric_limits<double>::infinity())
                    .distance_combine(std::plus<double>{})
                    .visitor(Visitor{to->second, incoming, cancellation}));
        } catch (const GoalReached&) { reached = true; }
        if (!reached) {
            result.status = RouteStatus::Unreachable;
            result.message = "no directed path between the supplied nodes";
            return finish();
        }
        std::vector<std::size_t> path_edges;
        Vertex current = to->second;
        while (current != from->second) {
            checkCancellation(cancellation);
            if (!incoming[current] || path_edges.size() >= input.nodes.size())
                throw std::logic_error("broken predecessor edge chain");
            const auto e = *incoming[current];
            path_edges.push_back(e);
            current = vertices.at(input.edges[e].from.value);
        }
        std::reverse(path_edges.begin(), path_edges.end());
        result.nodes.push_back(origin);
        for (const auto e : path_edges) {
            checkCancellation(cancellation);
            const auto& edge = input.edges[e];
            result.edges.push_back(edge.id);
            result.nodes.push_back(edge.to);
            result.total_distance_m += edge.length_m;
            // Keep supplied geometry, including tolerance-sized endpoint gaps.
            for (const auto point : edge.geometry) {
                checkCancellation(cancellation);
                result.geometry.push_back(point);
            }
        }
        if (path_edges.empty()) result.geometry.push_back(input.nodes[from->second].position);
        result.total_cost_m = distance[to->second];
        checkCancellation(cancellation);
        result.status = RouteStatus::Success;
    } catch (const SearchCancelled&) {
        result.status = RouteStatus::Cancelled;
        result.message = "route request cancelled";
    } catch (const std::exception& e) {
        result.status = RouteStatus::Failed;
        result.message = e.what();
    }
    if (result.status != RouteStatus::Success) {
        result.nodes.clear(); result.edges.clear(); result.geometry.clear();
        result.total_cost_m = 0; result.total_distance_m = 0;
    }
    return finish();
}
} // namespace p1::algorithms
