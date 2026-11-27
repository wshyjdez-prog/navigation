// Road network model contract (DEC-20261126-1-road-model-01, scheme A).
#include "TestSupport.h"
#include "core/BoostAStar.h"
#include "roadmodel/RoadModel.h"

#include <cmath>
#include <iostream>
#include <random>
#include <string>

using namespace p1::algorithms;
using namespace p1::algorithms::roadmodel;
using test_support::require;

namespace {
constexpr GeoPoint kWuhan{30.55, 114.32};

Input base() {
    Input in;
    in.metadata = {"test", "v1", "unit", {}};
    in.origin = kWuhan;
    return in;
}
// rows x cols grid, ~step_m apart, two-way, every edge usable by cars.
Input grid(int rows, int cols, double step_m) {
    auto in = base();
    const double dLat = step_m / 111195.0, dLon = dLat / std::cos(kWuhan.lat_deg * M_PI / 180);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            in.nodes.push_back({kWuhan.lat_deg + (r - rows / 2) * dLat, kWuhan.lon_deg + (c - cols / 2) * dLon});
    std::uint64_t id = 1;
    auto link = [&](std::uint32_t a, std::uint32_t b) {
        in.edges.push_back({model::EdgeId(id++), a, b, true});
        in.edges.push_back({model::EdgeId(id++), b, a, true});
    };
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const auto i = std::uint32_t(r * cols + c);
            if (c + 1 < cols) link(i, i + 1);
            if (r + 1 < rows) link(i, i + std::uint32_t(cols));
        }
    return in;
}
} // namespace

int main() {
    test_support::Runner test;
    {
        const std::string tag;

        test(tag + "projection round trip and surface distance", [&] {
            auto in = grid(2, 2, 100);
            const auto m = build(in);
            std::mt19937 rng(7);
            std::uniform_real_distribution<double> off(-0.45, 0.45);
            for (int i = 0; i < 200; ++i) {
                const GeoPoint g{kWuhan.lat_deg + off(rng), kWuhan.lon_deg + off(rng)};
                const auto back = m->unproject(m->project(g));
                require(m->distance_m(g, back) < 1e-6, "round trip error >= 1e-6 m within 50 km");
            }
            const auto o = m->project(kWuhan);
            require(std::hypot(o.x_m, o.y_m) < 1e-6, "origin must map to (0,0)");
            // One degree of latitude at 30.5 N: 110.85 km on WGS84, 111.19 km on the mean sphere.
            const double d = m->distance_m({30.0, 114.32}, {31.0, 114.32});
            require(d > 110700 && d < 111300, "1 degree latitude distance out of range");
            require(!m->graph().metadata.frame.definition.empty(), "frame definition missing");
        });

        test(tag + "one-way edges and directed reachability", [&] {
            auto in = base();
            in.nodes = {{30.55, 114.32}, {30.551, 114.32}, {30.552, 114.32}};
            in.edges = {{model::EdgeId(10), 0, 1, true}, {model::EdgeId(11), 1, 2, true}};
            const auto m = build(in);
            const auto p = m->route(0, 2, Profile::General);
            require(p.status == RouteStatus::Success, "forward route missing");
            require(p.edges == std::vector<model::EdgeId>{model::EdgeId(10), model::EdgeId(11)}, "wrong edge ids");
            require(std::abs(p.total_distance_m - m->distance_m(in.nodes[0], in.nodes[2])) < 1e-3, "wrong length");
            require(m->route(2, 0, Profile::General).status == RouteStatus::Unreachable, "one-way ignored");
            require(m->route(0, 9, Profile::General).status == RouteStatus::InvalidNode, "bad index accepted");
            require(m->graph().nodes.size() == 3 && m->graph().edges.size() == 2, "export size");
        });

        test(tag + "driving profile skips non-car edges and snaps to car nodes", [&] {
            auto in = grid(3, 3, 200);
            // Make the straight middle row (3-4-5) foot-only.
            for (auto& e : in.edges)
                if ((e.from == 3 && e.to == 4) || (e.from == 4 && e.to == 5) ||
                    (e.from == 4 && e.to == 3) || (e.from == 5 && e.to == 4)) e.car = false;
            const auto m = build(in);
            const auto general = m->route(3, 5, Profile::General);
            const auto car = m->route(3, 5, Profile::Driving);
            require(general.status == RouteStatus::Success && car.status == RouteStatus::Success, "route missing");
            require(car.total_distance_m > general.total_distance_m * 1.5, "driving used a foot-only edge");
            require(m->edge_count(Profile::Driving) + 4 == m->edge_count(Profile::General), "car edge count");
            // Node 4 (centre) still has car edges up/down, so it snaps in both profiles.
            require(m->snap(in.nodes[4], Profile::Driving, true, 50) == 4u, "driving snap");
            require(!m->snap({kWuhan.lat_deg + 0.05, kWuhan.lon_deg}, Profile::General, true, 500), "snap beyond limit");
        });

        test(tag + "snap honours direction (origin needs out-edge, destination in-edge)", [&] {
            auto in = base();
            in.nodes = {{30.55, 114.32}, {30.5509, 114.32}};
            in.edges = {{model::EdgeId(1), 0, 1, true}};
            const auto m = build(in);
            require(m->snap(in.nodes[1], Profile::General, true, 500) == 0u, "origin snapped to a sink");
            require(m->snap(in.nodes[0], Profile::General, false, 500) == 1u, "destination snapped to a source");
        });

        test(tag + "A* with surface bound equals legacy exhaustive A* cost", [&] {
            auto in = grid(30, 30, 120);
            std::mt19937 rng(11);
            std::uniform_real_distribution<double> jitter(-0.0003, 0.0003);
            for (auto& n : in.nodes) { n.lat_deg += jitter(rng); n.lon_deg += jitter(rng); }
            const auto m = build(in);
            std::uniform_int_distribution<std::uint32_t> pick(0, std::uint32_t(in.nodes.size() - 1));
            for (int i = 0; i < 40; ++i) {
                const auto a = pick(rng), b = pick(rng);
                const auto fast = m->route(a, b, Profile::General);
                const auto legacy = findRoute(m->graph(), model::NodeId(a + 1), model::NodeId(b + 1));
                require(fast.status == RouteStatus::Success && legacy.status == RouteStatus::Success, "route missing");
                require(std::abs(fast.total_cost_m - legacy.total_cost_m) <= 1e-6 * (1 + legacy.total_cost_m),
                        "heuristic search is not optimal");
            }
        });

        test(tag + "cancellation", [&] {
            auto flag = std::make_shared<std::atomic_bool>(true);
            bool threw = false;
            try { build(grid(10, 10, 100), flag); } catch (const std::runtime_error&) { threw = true; }
            require(threw, "build ignored cancellation");
            const auto m = build(grid(60, 60, 100));
            const auto p = m->route(0, 3599, Profile::General, flag);
            require(p.status == RouteStatus::Cancelled && p.edges.empty(), "route ignored cancellation");
        });

        test(tag + "invalid input", [&] {
            auto in = base();
            in.nodes = {{30.55, 114.32}, {91, 114.32}};
            in.edges = {{model::EdgeId(1), 0, 1, true}};
            bool threw = false;
            try { build(in); } catch (const std::runtime_error&) { threw = true; }
            require(threw, "invalid latitude accepted");
            in.nodes[1] = in.nodes[0];
            threw = false;
            try { build(in); } catch (const std::runtime_error&) { threw = true; }
            require(threw, "graph with only zero-length edges accepted");
        });
    }
    return test.finish();
}
