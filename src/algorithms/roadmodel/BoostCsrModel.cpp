// Road model, scheme A (DEC-20261126-1-road-model-01): Boost only.
// Projection: Boost.Geometry srs (PROJ port) transverse Mercator at the
// network centre. Distance: Boost.Geometry haversine on the IUGG mean sphere.
// Graph: Boost.Graph compressed_sparse_row_graph, built once per network.
// The graph type must be declared before the shared template (two-phase lookup).
#include <boost/graph/compressed_sparse_row_graph.hpp>
#include "roadmodel/detail/ModelImpl.h"

#include <boost/geometry/algorithms/distance.hpp>
#include <boost/geometry/srs/projection.hpp>
#include <boost/geometry/strategies/spherical/distance_haversine.hpp>

#include <cstdio>

namespace p1::algorithms::roadmodel {
namespace {
namespace bg = boost::geometry;
constexpr double kMeanEarthRadiusM = 6371008.8;

std::string tmerc(GeoPoint o) {
    char text[160];
    std::snprintf(text, sizeof text, "+proj=tmerc +ellps=WGS84 +lat_0=%.12f +lon_0=%.12f +k=1 +x_0=0 +y_0=0 +units=m",
                  o.lat_deg, o.lon_deg);
    return text;
}

struct BoostGeo {
    static constexpr const char* name = "Boost.Graph CSR + Boost.Geometry tmerc/haversine";
    using LonLat = bg::model::point<double, 2, bg::cs::geographic<bg::degree>>;
    using Sphere = bg::model::point<double, 2, bg::cs::spherical_equatorial<bg::degree>>;
    using XY = bg::model::point<double, 2, bg::cs::cartesian>;
    std::string def;
    bg::srs::projection<> proj;
    bg::strategy::distance::haversine<double> haversine{kMeanEarthRadiusM};

    explicit BoostGeo(GeoPoint origin) : def(tmerc(origin)), proj(bg::srs::proj4(def)) {}
    model::MetricPoint forward(GeoPoint g) const {
        XY xy;
        proj.forward(LonLat(g.lon_deg, g.lat_deg), xy);
        return {bg::get<0>(xy), bg::get<1>(xy)};
    }
    GeoPoint reverse(model::MetricPoint m) const {
        LonLat ll;
        proj.inverse(XY(m.x_m, m.y_m), ll);
        return {bg::get<1>(ll), bg::get<0>(ll)};
    }
    double distance(GeoPoint a, GeoPoint b) const {
        return bg::distance(Sphere(a.lon_deg, a.lat_deg), Sphere(b.lon_deg, b.lat_deg), haversine);
    }
    std::string definition() const { return "Boost.Geometry srs " + def + "; haversine R=6371008.8 m"; }
};

// Default (size_t) vertex/edge index types: filtered_graph's vertex_index map
// assumes size_t, a 32-bit CSR index does not compile with it in Boost 1.83.
using Graph = boost::compressed_sparse_row_graph<boost::directedS, boost::no_property, detail::EdgeProp>;
} // namespace

std::shared_ptr<const RoadModel> build(Input input, const Cancellation& c) {
    return std::make_shared<const detail::Model<BoostGeo, Graph>>(std::move(input), c,
        [](std::size_t n, const auto& pairs, const auto& props) {
            return Graph(boost::edges_are_unsorted_multi_pass, pairs.begin(), pairs.end(), props.begin(), n);
        });
}

} // namespace p1::algorithms::roadmodel
