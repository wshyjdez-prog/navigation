#include "core/BoostAStar.h"
#include "samples/FixedRoadNetwork.h"
#include <boost/version.hpp>
#include <iostream>

int main() {
    using namespace p1::algorithms;
    const auto result = findRoute(samples::fixedRoadNetwork(), model::NodeId{900}, model::NodeId{5});
    std::cout << "Synthetic metre fixture, real Boost.Graph astar_search, Boost " << BOOST_LIB_VERSION << '\n';
    if (result.status != RouteStatus::Success) { std::cerr << result.message << '\n'; return 1; }
    std::cout << "nodes:";
    for (const auto id : result.nodes) std::cout << ' ' << id.value;
    std::cout << "\nedges:";
    for (const auto id : result.edges) std::cout << ' ' << id.value;
    std::cout << "\ncost_m=" << result.total_cost_m << " distance_m=" << result.total_distance_m
              << " elapsed_ms=" << result.elapsed_ms << '\n';
    return 0;
}
