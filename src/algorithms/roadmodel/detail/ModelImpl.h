#pragma once
// Model body (scheme A instantiates it in BoostCsrModel.cpp). It needs:
//   Geo   : explicit Geo(GeoPoint origin); MetricPoint forward(GeoPoint) const;
//           GeoPoint reverse(MetricPoint) const; double distance(GeoPoint, GeoPoint) const;
//           std::string definition() const; static constexpr const char* name;
//   Graph : a Boost.Graph directed graph with bundled EdgeProp and vertex_index,
//           built by make(node_count, endpoint pairs, properties).
// Open-source parts: Boost.Graph (graph container, filtered_graph, astar_search)
// and Boost.Geometry (R*-tree nearest-node index); the backend adds projection
// and surface distance.
#include "model/Validation.h"
#include "roadmodel/RoadModel.h"

#include <boost/geometry.hpp> // self-contained: cartesian strategies for the R*-tree
#include <boost/geometry/index/rtree.hpp>
#include <boost/graph/astar_search.hpp>
#include <boost/graph/filtered_graph.hpp>

#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace p1::algorithms::roadmodel::detail {

struct EdgeProp {
    std::uint32_t input{}; // index into graph_.edges
    double cost{};
    bool car{};
};

inline void checkBuild(const Cancellation& c) {
    if (c && c->load(std::memory_order_relaxed)) throw std::runtime_error("已取消");
}

template <class Geo, class Graph>
class Model final : public RoadModel {
public:
    using Vertex = typename boost::graph_traits<Graph>::vertex_descriptor;
    using BPoint = boost::geometry::model::point<double, 2, boost::geometry::cs::cartesian>;
    using Value = std::pair<BPoint, std::uint32_t>;
    using Tree = boost::geometry::index::rtree<Value, boost::geometry::index::rstar<16>>;
    enum : std::uint8_t { Out = 1, In = 2, CarOut = 4, CarIn = 8 };

    template <class MakeGraph>
    Model(Input input, const Cancellation& c, MakeGraph make)
        : geo_(input.origin), nodes_(std::move(input.nodes)) {
        const auto n = nodes_.size();
        if (n == 0 || n > std::numeric_limits<std::uint32_t>::max() - 1)
            throw std::runtime_error("路网节点数无效");
        graph_.metadata = std::move(input.metadata);
        graph_.metadata.frame = {model::CoordinateSystem::LocalCartesian, model::LengthUnit::Metres,
                                 geo_.definition()};
        graph_.nodes.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            if (i % 4096 == 0) checkBuild(c);
            const auto& g = nodes_[i];
            if (!std::isfinite(g.lat_deg) || !std::isfinite(g.lon_deg) || std::abs(g.lat_deg) > 90 ||
                std::abs(g.lon_deg) > 180)
                throw std::runtime_error("路网包含非法 WGS84 坐标");
            graph_.nodes.push_back({model::NodeId(i + 1), geo_.forward(g)});
        }
        std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
        std::vector<EdgeProp> props;
        pairs.reserve(input.edges.size());
        props.reserve(input.edges.size());
        graph_.edges.reserve(input.edges.size());
        flags_.assign(n, 0);
        for (std::size_t i = 0; i < input.edges.size(); ++i) {
            if (i % 4096 == 0) checkBuild(c);
            const auto& e = input.edges[i];
            if (e.from >= n || e.to >= n) throw std::runtime_error("路网边引用了不存在的节点");
            if (e.from == e.to) continue;
            const double length = geo_.distance(nodes_[e.from], nodes_[e.to]);
            if (!(length > 0)) continue;
            const auto index = static_cast<std::uint32_t>(graph_.edges.size());
            graph_.edges.push_back({e.id, model::NodeId(e.from + 1), model::NodeId(e.to + 1),
                                    {graph_.nodes[e.from].position, graph_.nodes[e.to].position},
                                    length, length});
            pairs.emplace_back(e.from, e.to);
            props.push_back({index, length, e.car});
            flags_[e.from] |= Out;
            flags_[e.to] |= In;
            if (e.car) {
                flags_[e.from] |= CarOut;
                flags_[e.to] |= CarIn;
                ++carEdges_;
            }
        }
        if (graph_.edges.empty()) throw std::runtime_error("路网没有有效的非零长度边");
        checkBuild(c);
        if (!model::validateGraph(graph_).ok()) throw std::runtime_error("路网模型校验失败");
        graph_store_ = make(n, pairs, props);
        checkBuild(c);
        std::vector<Value> values;
        values.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i)
            if (flags_[i]) {
                const auto& p = graph_.nodes[i].position;
                values.emplace_back(BPoint(p.x_m, p.y_m), i);
            }
        tree_ = Tree(values.begin(), values.end()); // packing (bulk) load
    }

    const char* backend() const noexcept override { return Geo::name; }
    model::MetricPoint project(GeoPoint g) const override { return geo_.forward(g); }
    GeoPoint unproject(model::MetricPoint m) const override { return geo_.reverse(m); }
    double distance_m(GeoPoint a, GeoPoint b) const override { return geo_.distance(a, b); }
    std::size_t edge_count(Profile p) const override {
        return p == Profile::Driving ? carEdges_ : graph_.edges.size();
    }

    std::optional<std::uint32_t> snap(GeoPoint geo, Profile profile, bool origin,
                                      double max_m) const override {
        if (!std::isfinite(geo.lat_deg) || !std::isfinite(geo.lon_deg)) return std::nullopt;
        const std::uint8_t need = profile == Profile::Driving ? (origin ? CarOut : CarIn)
                                                              : (origin ? Out : In);
        const auto p = geo_.forward(geo);
        std::vector<Value> hit;
        namespace bgi = boost::geometry::index;
        tree_.query(bgi::nearest(BPoint(p.x_m, p.y_m), 1) &&
                        bgi::satisfies([&](const Value& v) { return (flags_[v.second] & need) != 0; }),
                    std::back_inserter(hit));
        if (hit.empty() || geo_.distance(geo, nodes_[hit.front().second]) > max_m) return std::nullopt;
        return hit.front().second;
    }

    RoutePath route(std::uint32_t from, std::uint32_t to, Profile profile,
                    const Cancellation& cancellation) const override {
        if (profile == Profile::Driving)
            return search(boost::make_filtered_graph(graph_store_, CarOnly{&graph_store_}), from, to,
                          cancellation);
        return search(graph_store_, from, to, cancellation);
    }

private:
    struct CarOnly {
        const Graph* g{};
        template <class E> bool operator()(const E& e) const { return (*g)[e].car; }
    };
    struct GoalReached {};
    struct Cancelled {};

    template <class G>
    struct Heuristic : boost::astar_heuristic<G, double> {
        const Geo* geo;
        const std::vector<GeoPoint>* nodes;
        GeoPoint goal;
        Heuristic(const Geo* g, const std::vector<GeoPoint>* n, GeoPoint t) : geo(g), nodes(n), goal(t) {}
        // Edge costs are this same surface metric, so the straight-line bound
        // never exceeds the remaining path cost (admissible).
        double operator()(typename boost::graph_traits<G>::vertex_descriptor v) const {
            return geo->distance((*nodes)[v], goal) * (1.0 - 1e-9);
        }
    };

    template <class G>
    struct Visitor : boost::default_astar_visitor {
        typename boost::graph_traits<G>::vertex_descriptor goal;
        std::vector<std::uint32_t>* incoming;
        const Cancellation* cancellation;
        const Graph* base; // edge descriptors are shared with filtered views
        mutable unsigned ticks = 0;
        template <class V> void examine_vertex(V v, const G&) const {
            if ((++ticks & 255u) == 0 && *cancellation && (*cancellation)->load(std::memory_order_relaxed))
                throw Cancelled{};
            if (v == goal) throw GoalReached{};
        }
        template <class E> void edge_relaxed(E e, const G& g) const { (*incoming)[boost::target(e, g)] = (*base)[e].input; }
    };

    template <class G>
    RoutePath search(const G& g, std::uint32_t from, std::uint32_t to, const Cancellation& cancellation) const {
        const auto started = std::chrono::steady_clock::now();
        RoutePath result;
        result.dataset_id = graph_.metadata.dataset_id;
        result.data_version = graph_.metadata.version;
        const auto n = nodes_.size();
        try {
            if (from >= n || to >= n) {
                result.status = RouteStatus::InvalidNode;
                result.message = "origin or destination node index does not exist";
            } else {
                std::vector<double> distance(n, std::numeric_limits<double>::infinity());
                std::vector<double> rank(n, std::numeric_limits<double>::infinity());
                std::vector<boost::default_color_type> color(n, boost::white_color);
                std::vector<std::uint32_t> incoming(n, std::numeric_limits<std::uint32_t>::max());
                const auto index = boost::get(boost::vertex_index, graph_store_);
                bool reached = from == to;
                if (!reached) {
                    try {
                        boost::astar_search(g, Vertex(from), Heuristic<G>(&geo_, &nodes_, nodes_[to]),
                            boost::weight_map(boost::get(&EdgeProp::cost, graph_store_))
                                .distance_map(boost::make_iterator_property_map(distance.begin(), index))
                                .rank_map(boost::make_iterator_property_map(rank.begin(), index))
                                .color_map(boost::make_iterator_property_map(color.begin(), index))
                                .distance_inf(std::numeric_limits<double>::infinity())
                                .visitor(Visitor<G>{{}, Vertex(to), &incoming, &cancellation, &graph_store_}));
                    } catch (const GoalReached&) { reached = true; }
                }
                if (!reached) {
                    result.status = RouteStatus::Unreachable;
                    result.message = "no directed path between the supplied nodes";
                } else {
                    std::vector<std::uint32_t> path;
                    for (std::uint32_t v = to; v != from;) {
                        const auto e = incoming[v];
                        if (e == std::numeric_limits<std::uint32_t>::max() || path.size() >= n)
                            throw std::logic_error("broken predecessor edge chain");
                        path.push_back(e);
                        v = static_cast<std::uint32_t>(graph_.edges[e].from.value - 1);
                    }
                    result.nodes.push_back(model::NodeId(from + 1));
                    for (auto it = path.rbegin(); it != path.rend(); ++it) {
                        const auto& edge = graph_.edges[*it];
                        result.edges.push_back(edge.id);
                        result.nodes.push_back(edge.to);
                        result.total_distance_m += edge.length_m;
                        result.geometry.insert(result.geometry.end(), edge.geometry.begin(), edge.geometry.end());
                    }
                    if (path.empty()) result.geometry.push_back(graph_.nodes[from].position);
                    result.total_cost_m = from == to ? 0.0 : distance[to];
                    result.status = RouteStatus::Success;
                }
            }
        } catch (const Cancelled&) {
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
        result.elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        return result;
    }

    Geo geo_;
    std::vector<GeoPoint> nodes_;
    std::vector<std::uint8_t> flags_;
    std::size_t carEdges_ = 0;
    Graph graph_store_;
    Tree tree_;
};

} // namespace p1::algorithms::roadmodel::detail
