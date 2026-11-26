#pragma once
// Road network model (DEC-20261126-1-road-model-01, scheme A). Built once per
// loaded network from an immutable OSMX snapshot; every query is const and may
// run on any worker concurrently. Open-source parts: Boost.Graph
// compressed_sparse_row_graph + filtered_graph + astar_search, Boost.Geometry
// R*-tree (nearest node), srs transverse Mercator and haversine (Boost 1.83).
// Node indices are OSMX node indices; the exported GraphData uses
// NodeId = index + 1 and the caller-supplied EdgeId, so FMM shapefiles and the
// legacy findRoute(GraphData) keep their ids.
#include "core/BoostAStar.h"
#include "model/Types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace p1::algorithms::roadmodel {

struct GeoPoint {
    double lat_deg{};
    double lon_deg{};
};

struct InputEdge {
    model::EdgeId id;
    std::uint32_t from{}; // node index
    std::uint32_t to{};   // node index
    bool car{};           // usable by RouteMode::Driving (rule owned by the app)
};

struct Input {
    model::DatasetMetadata metadata; // frame is filled in by the model
    GeoPoint origin;                 // projection origin (network centre)
    std::vector<GeoPoint> nodes;
    std::vector<InputEdge> edges;    // from != to; zero-length edges are dropped
};

enum class Profile { General, Driving };

class RoadModel {
public:
    virtual ~RoadModel() = default;
    virtual const char* backend() const noexcept = 0;
    virtual model::MetricPoint project(GeoPoint geo) const = 0;
    virtual GeoPoint unproject(model::MetricPoint metric) const = 0;
    // Surface distance used for edge lengths and as the A* lower bound.
    virtual double distance_m(GeoPoint a, GeoPoint b) const = 0;
    // Nearest node within max_m that can start (origin) or end a route.
    virtual std::optional<std::uint32_t> snap(GeoPoint geo, Profile profile, bool origin,
                                              double max_m) const = 0;
    virtual RoutePath route(std::uint32_t from, std::uint32_t to, Profile profile,
                            const Cancellation& cancellation = {}) const = 0;
    virtual std::size_t edge_count(Profile profile) const = 0;
    // Projected copy for FMM export and the legacy GraphData A*.
    const model::GraphData& graph() const noexcept { return graph_; }

protected:
    model::GraphData graph_;
};

// Throws std::runtime_error on invalid input or cancellation ("已取消").
std::shared_ptr<const RoadModel> build(Input input, const Cancellation& cancellation = {});

} // namespace p1::algorithms::roadmodel
