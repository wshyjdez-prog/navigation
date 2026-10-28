#include "model/Validation.h"

#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

using namespace p1::algorithms::model;

static_assert(!std::is_convertible_v<NodeId, EdgeId>);
static_assert(!std::is_convertible_v<NodeId, NodeIndex>);
static_assert(!std::is_convertible_v<EdgeId, EdgeIndex>);
static_assert(!std::is_convertible_v<NodeIndex, EdgeIndex>);
static_assert(!std::is_convertible_v<std::uint64_t, NodeId>);
static_assert(std::is_same_v<decltype(std::declval<GraphSnapshot&>().data()), const GraphData&>);
static_assert(std::is_same_v<decltype(std::declval<TrajectorySnapshot&>().data()), const TrajectoryData&>);
static_assert(!std::is_constructible_v<GraphSnapshot, GraphData&&>);
static_assert(!std::is_constructible_v<TrajectorySnapshot, TrajectoryData&&>);

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

DatasetMetadata metadata() {
    return {"member3-fixed-model", "fixture-v1", "synthetic model test",
            {CoordinateSystem::LocalCartesian, LengthUnit::Metres,
             "synthetic origin (0,0), x east, y north, metres"}};
}

GraphData graph() {
    // Sparse IDs and reverse vector order expose accidental ID-as-index use.
    return {metadata(), {{NodeId{900}, {10, 0}}, {NodeId{42}, {0, 0}}},
            {{EdgeId{7000}, NodeId{42}, NodeId{900}, {{0, 0}, {10, 0}}, 10, 10}}};
}

TrajectoryData trajectory() {
    return {metadata(), {{7, {0, 0}, 1000, std::nullopt},
                         {11, {10, 0}, 2000, 2.0}}};
}

void expectErrors(const ValidationResult& result,
                  std::initializer_list<ValidationError> expected) {
    require(result.errors.size() == expected.size(), "wrong error count");
    std::size_t i = 0;
    for (const auto& e : expected) {
        const auto& actual = result.errors[i++];
        require(actual.code == e.code, "wrong error category");
        require(actual.object == e.object, "wrong object kind");
        require(actual.index == e.index, "wrong object position");
        require(actual.geometry_point_index == e.geometry_point_index, "wrong geometry point position");
        require(actual.field == e.field, "wrong field");
    }
}

int passed = 0;
int failed = 0;

void test(const std::string& name, const std::function<void()>& body) {
    try {
        body();
        ++passed;
        std::cout << "PASS " << name << '\n';
    } catch (const std::exception& e) {
        ++failed;
        std::cerr << "FAIL " << name << ": " << e.what() << '\n';
    }
}

void badGraph(const std::string& name, const std::function<void(GraphData&)>& change,
              std::initializer_list<ValidationError> expected) {
    test(name, [&] {
        auto input = graph();
        change(input);
        expectErrors(validateGraph(input), expected);
        const auto published = publishGraph(input);
        require(!published.snapshot, "invalid graph published");
        expectErrors(published.validation, expected);
    });
}

void badTrajectory(const std::string& name, const std::function<void(TrajectoryData&)>& change,
                   std::initializer_list<ValidationError> expected) {
    test(name, [&] {
        auto input = trajectory();
        change(input);
        expectErrors(validateTrajectory(input), expected);
        const auto published = publishTrajectory(input);
        require(!published.snapshot, "invalid trajectory published");
        expectErrors(published.validation, expected);
    });
}

} // namespace

int main() {
    test("valid sparse IDs, metadata and directed edge", [] {
        auto input = graph();
        require(validateGraph(input).ok(), "valid graph rejected");
        const auto published = publishGraph(input);
        require(published.snapshot && published.validation.ok(), "valid graph not published");
        const auto& data = published.snapshot->data();
        require(data.metadata.dataset_id == "member3-fixed-model" &&
                data.metadata.version == "fixture-v1" && data.metadata.source == "synthetic model test",
                "metadata changed");
        require(data.metadata.frame.system == CoordinateSystem::LocalCartesian &&
                data.metadata.frame.unit == LengthUnit::Metres &&
                data.metadata.frame.definition == input.metadata.frame.definition, "frame changed");
        require(data.edges.size() == 1 && data.edges[0].from == NodeId{42} &&
                data.edges[0].to == NodeId{900}, "edge direction changed or reverse edge invented");
    });
    test("isolated node, zero ID and no edges", [] {
        auto input = graph();
        input.nodes = {{NodeId{0}, {0, 0}}};
        input.edges.clear();
        require(validateGraph(input).ok(), "isolated node rejected");
    });
    test("parallel, reverse and self-loop edges", [] {
        auto input = graph();
        input.edges.push_back({EdgeId{1}, NodeId{42}, NodeId{900}, {{0, 0}, {10, 0}}, 10, 0});
        input.edges.push_back({EdgeId{2}, NodeId{900}, NodeId{42}, {{10, 0}, {0, 0}}, 10, 10});
        input.edges.push_back({EdgeId{3}, NodeId{42}, NodeId{42}, {{0, 0}, {1, 0}, {0, 0}}, 2, 2});
        require(validateGraph(input).ok(), "valid directed multigraph rejected");
    });
    test("repeated geometry points retained", [] {
        auto input = graph();
        input.edges[0].geometry = {{0, 0}, {0, 0}, {10, 0}, {10, 0}};
        const auto p = publishGraph(input);
        require(p.snapshot && p.snapshot->data().edges[0].geometry.size() == 4,
                "repeated geometry removed");
    });
    test("endpoint tolerance boundary retained", [] {
        auto input = graph();
        input.edges[0].geometry.front().y_m = endpoint_tolerance_m;
        const auto p = publishGraph(input);
        require(p.snapshot && p.snapshot->data().edges[0].geometry.front().y_m == endpoint_tolerance_m,
                "boundary rejected or snapped");
    });
    // Review T1: the end boundary must use the same inclusive tolerance.
    test("to endpoint tolerance boundary retained", [] {
        auto input = graph();
        input.edges[0].geometry.back().y_m = endpoint_tolerance_m;
        const auto p = publishGraph(input);
        require(p.snapshot && p.validation.ok() &&
                p.snapshot->data().edges[0].geometry.back().y_m == endpoint_tolerance_m,
                "to boundary rejected or snapped");
    });
    // Review T2: opaque versions are checked for blankness, never trimmed.
    test("nonblank opaque version whitespace retained", [] {
        const std::string version = " \topaque-v1 \r\n";
        auto g = graph();
        auto t = trajectory();
        g.metadata.version = version;
        t.metadata.version = version;
        const auto gp = publishGraph(g);
        const auto tp = publishTrajectory(t);
        require(gp.snapshot && gp.validation.ok() && tp.snapshot && tp.validation.ok(),
                "nonblank opaque version rejected");
        require(gp.snapshot->data().metadata.version == version &&
                tp.snapshot->data().metadata.version == version,
                "opaque version whitespace changed");
    });
    test("projected frame with explicit definition", [] {
        auto input = graph();
        input.metadata.frame = {CoordinateSystem::ProjectedCartesian, LengthUnit::Metres,
                                "synthetic projected CRS, identity transform"};
        require(validateGraph(input).ok(), "projected frame rejected");
    });
    test("valid trajectory, gaps and optional accuracy", [] {
        const auto p = publishTrajectory(trajectory());
        require(p.snapshot && p.validation.ok(), "valid trajectory rejected");
        const auto& data = p.snapshot->data();
        require(data.points[0].original_index == 7 && data.points[1].original_index == 11,
                "original order/indices changed");
        require(!data.points[0].accuracy_m && data.points[1].accuracy_m == 2.0,
                "optional accuracy changed");
        require(data.metadata.version == "fixture-v1", "trajectory version lost");
    });
    test("equal timestamps, repeated position, zero accuracy", [] {
        auto input = trajectory();
        input.points[1].unix_time_ms = input.points[0].unix_time_ms;
        input.points[1].position = input.points[0].position;
        input.points[1].accuracy_m = 0;
        const auto p = publishTrajectory(input);
        require(p.snapshot && p.snapshot->data().points.size() == 2, "equal times rejected or merged");
        require(p.snapshot->data().points[1].unix_time_ms == 1000 &&
                p.snapshot->data().points[1].position.x_m == 0, "trajectory changed");
    });
    test("single point and pre-epoch timestamps", [] {
        auto input = trajectory();
        input.points.resize(1);
        input.points[0].unix_time_ms = -1;
        require(validateTrajectory(input).ok(), "valid Unix timestamp rejected");
    });
    test("graph snapshot isolates aliases including rvalue publication", [] {
        auto input = graph();
        auto& node = input.nodes[0];
        auto& geometry = input.edges[0].geometry[0];
        auto& version = input.metadata.version;
        const auto p = publishGraph(std::move(input)); // Must still copy.
        require(p.snapshot != nullptr, "publication failed");
        node.position.x_m = 999;
        geometry.y_m = 999;
        version = "changed";
        input.edges[0].cost_m = 999;
        const auto& data = p.snapshot->data();
        require(data.nodes[0].position.x_m == 10 && data.edges[0].geometry[0].y_m == 0 &&
                data.metadata.version == "fixture-v1" && data.edges[0].cost_m == 10, "writable graph alias leaked");
        input.nodes.clear();
        require(data.nodes.size() == 2, "snapshot depended on input storage");
    });
    test("trajectory snapshot survives source lifetime and updates", [] {
        std::shared_ptr<const TrajectorySnapshot> retained;
        {
            auto input = trajectory();
            auto& point = input.points[0];
            auto& definition = input.metadata.frame.definition;
            retained = publishTrajectory(std::move(input)).snapshot;
            require(retained != nullptr, "publication failed");
            point.position.x_m = 99;
            point.accuracy_m = 99;
            definition = "changed";
            input.metadata.version = "fixture-v2";
            const auto updated = publishTrajectory(input);
            require(updated.snapshot && updated.snapshot->data().metadata.version == "fixture-v2",
                    "new version publication failed");
        }
        const auto& data = retained->data();
        require(data.points[0].position.x_m == 0 && !data.points[0].accuracy_m &&
                data.metadata.version == "fixture-v1" && data.metadata.frame.definition == metadata().frame.definition,
                "writable trajectory alias leaked");
    });

    badGraph("empty graph", [](auto& g) { g.nodes.clear(); g.edges.clear(); },
             {{ErrorCode::EmptyGraph, ObjectKind::Graph, {}, {}, "nodes"}});
    badTrajectory("empty trajectory", [](auto& t) { t.points.clear(); },
                  {{ErrorCode::EmptyTrajectory, ObjectKind::Trajectory, {}, {}, "points"}});
    badGraph("missing metadata", [](auto& g) { g.metadata = {}; },
             {{ErrorCode::MissingMetadata, ObjectKind::Metadata, {}, {}, "dataset_id"},
              {ErrorCode::MissingMetadata, ObjectKind::Metadata, {}, {}, "version"},
              {ErrorCode::MissingMetadata, ObjectKind::Metadata, {}, {}, "source"},
              {ErrorCode::MissingMetadata, ObjectKind::Metadata, {}, {}, "frame.definition"}});
    badTrajectory("blank metadata", [](auto& t) { t.metadata.source = " \t\n"; },
                  {{ErrorCode::MissingMetadata, ObjectKind::Metadata, {}, {}, "source"}});
    // Review T2: isolate each required field and its metadata location.
    struct BlankMetadataCase {
        const char* field;
        std::function<void(DatasetMetadata&)> set_blank;
    };
    const BlankMetadataCase blank_metadata_cases[] = {
        {"dataset_id", [](auto& m) { m.dataset_id = " \t\r\n\f\v"; }},
        {"version", [](auto& m) { m.version = " \t\r\n\f\v"; }},
        {"source", [](auto& m) { m.source = " \t\r\n\f\v"; }},
        {"frame.definition", [](auto& m) { m.frame.definition = " \t\r\n\f\v"; }}
    };
    for (const auto& entry : blank_metadata_cases) {
        badGraph(std::string("blank metadata field ") + entry.field,
                 [&](auto& g) { entry.set_blank(g.metadata); },
                 {{ErrorCode::MissingMetadata, ObjectKind::Metadata, {}, {}, entry.field}});
    }
    badGraph("unsupported coordinate system", [](auto& g) { g.metadata.frame.system = static_cast<CoordinateSystem>(99); },
             {{ErrorCode::UnsupportedCoordinateSystem, ObjectKind::Metadata, {}, {}, "frame.system"}});
    badTrajectory("unsupported unit", [](auto& t) { t.metadata.frame.unit = static_cast<LengthUnit>(99); },
                  {{ErrorCode::UnsupportedLengthUnit, ObjectKind::Metadata, {}, {}, "frame.unit"}});
    badGraph("duplicate node ID", [](auto& g) { g.nodes.push_back(g.nodes[0]); },
             {{ErrorCode::DuplicateNodeId, ObjectKind::Node, 2, {}, "id"}});
    badGraph("duplicate edge ID", [](auto& g) { g.edges.push_back(g.edges[0]); },
             {{ErrorCode::DuplicateEdgeId, ObjectKind::Edge, 1, {}, "id"}});
    badGraph("missing from endpoint", [](auto& g) { g.edges[0].from = NodeId{123}; },
             {{ErrorCode::MissingEndpoint, ObjectKind::Edge, 0, {}, "from"}});
    badGraph("missing to endpoint", [](auto& g) { g.edges[0].to = NodeId{123}; },
             {{ErrorCode::MissingEndpoint, ObjectKind::Edge, 0, {}, "to"}});
    // Review T4: report both endpoints, in order, and refuse publication.
    badGraph("both endpoints missing", [](auto& g) {
        g.edges[0].from = NodeId{123};
        g.edges[0].to = NodeId{456};
    }, {{ErrorCode::MissingEndpoint, ObjectKind::Edge, 0, {}, "from"},
        {ErrorCode::MissingEndpoint, ObjectKind::Edge, 0, {}, "to"}});
    badGraph("negative length", [](auto& g) { g.edges[0].length_m = -1; },
             {{ErrorCode::NegativeValue, ObjectKind::Edge, 0, {}, "length_m"}});
    badGraph("negative cost", [](auto& g) { g.edges[0].cost_m = -1; },
             {{ErrorCode::NegativeValue, ObjectKind::Edge, 0, {}, "cost_m"}});
    badGraph("zero length", [](auto& g) { g.edges[0].length_m = 0; },
             {{ErrorCode::ZeroLengthEdge, ObjectKind::Edge, 0, {}, "length_m"}});
    badGraph("negative zero length", [](auto& g) { g.edges[0].length_m = -0.0; },
             {{ErrorCode::ZeroLengthEdge, ObjectKind::Edge, 0, {}, "length_m"}});
    badGraph("empty geometry", [](auto& g) { g.edges[0].geometry.clear(); },
             {{ErrorCode::TooFewGeometryPoints, ObjectKind::Edge, 0, {}, "geometry"}});
    badGraph("single geometry point", [](auto& g) { g.edges[0].geometry = {{0, 0}}; },
             {{ErrorCode::TooFewGeometryPoints, ObjectKind::Edge, 0, {}, "geometry"},
              {ErrorCode::EndpointMismatch, ObjectKind::Edge, 0, 0, "to"}});
    badGraph("all repeated geometry", [](auto& g) { g.edges[0].to = NodeId{42}; g.edges[0].geometry = {{0, 0}, {0, 0}}; },
             {{ErrorCode::DegenerateGeometry, ObjectKind::Edge, 0, {}, "geometry"}});
    badGraph("reversed geometry", [](auto& g) { g.edges[0].geometry = {{10, 0}, {0, 0}}; },
             {{ErrorCode::EndpointMismatch, ObjectKind::Edge, 0, 0, "from"},
              {ErrorCode::EndpointMismatch, ObjectKind::Edge, 0, 1, "to"}});
    badGraph("outside from tolerance", [](auto& g) { g.edges[0].geometry[0].y_m = 2 * endpoint_tolerance_m; },
             {{ErrorCode::EndpointMismatch, ObjectKind::Edge, 0, 0, "from"}});
    badGraph("outside to tolerance", [](auto& g) { g.edges[0].geometry[1].y_m = 2 * endpoint_tolerance_m; },
             {{ErrorCode::EndpointMismatch, ObjectKind::Edge, 0, 1, "to"}});
    // Review T1: individually small components can exceed the radial bound.
    badGraph("diagonal from error outside tolerance", [](auto& g) {
        g.edges[0].geometry.front() = {0.8 * endpoint_tolerance_m, 0.8 * endpoint_tolerance_m};
    }, {{ErrorCode::EndpointMismatch, ObjectKind::Edge, 0, 0, "from"}});
    badTrajectory("decreasing time", [](auto& t) { t.points[1].unix_time_ms = 999; },
                  {{ErrorCode::TimestampDecreased, ObjectKind::TrajectoryPoint, 1, {}, "unix_time_ms"}});
    badTrajectory("duplicate original index", [](auto& t) { t.points[1].original_index = 7; },
                  {{ErrorCode::OriginalIndexNotIncreasing, ObjectKind::TrajectoryPoint, 1, {}, "original_index"}});
    badTrajectory("decreasing original index", [](auto& t) { t.points[1].original_index = 6; },
                  {{ErrorCode::OriginalIndexNotIncreasing, ObjectKind::TrajectoryPoint, 1, {}, "original_index"}});
    badTrajectory("negative accuracy", [](auto& t) { t.points[0].accuracy_m = -1; },
                  {{ErrorCode::NegativeValue, ObjectKind::TrajectoryPoint, 0, {}, "accuracy_m"}});

    const double invalid_values[] = {std::numeric_limits<double>::quiet_NaN(),
                                     std::numeric_limits<double>::infinity(),
                                     -std::numeric_limits<double>::infinity()};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto value = invalid_values[i];
        const auto suffix = std::to_string(i);
        badGraph("nonfinite node " + suffix, [=](auto& g) { g.nodes[0].position.x_m = value; },
                 {{ErrorCode::NonFiniteCoordinate, ObjectKind::Node, 0, {}, "x_m"}});
        badGraph("nonfinite geometry " + suffix, [=](auto& g) { g.edges[0].geometry[1].y_m = value; },
                 {{ErrorCode::NonFiniteCoordinate, ObjectKind::Edge, 0, 1, "y_m"}});
        badGraph("nonfinite length " + suffix, [=](auto& g) { g.edges[0].length_m = value; },
                 {{ErrorCode::NonFiniteValue, ObjectKind::Edge, 0, {}, "length_m"}});
        badGraph("nonfinite cost " + suffix, [=](auto& g) { g.edges[0].cost_m = value; },
                 {{ErrorCode::NonFiniteValue, ObjectKind::Edge, 0, {}, "cost_m"}});
        badTrajectory("nonfinite position " + suffix, [=](auto& t) { t.points[1].position.y_m = value; },
                      {{ErrorCode::NonFiniteCoordinate, ObjectKind::TrajectoryPoint, 1, {}, "y_m"}});
        badTrajectory("nonfinite accuracy " + suffix, [=](auto& t) { t.points[0].accuracy_m = value; },
                      {{ErrorCode::NonFiniteValue, ObjectKind::TrajectoryPoint, 0, {}, "accuracy_m"}});
        // Review T3: complete the coordinate axes for every nonfinite value.
        badGraph("nonfinite node y " + suffix, [=](auto& g) { g.nodes[0].position.y_m = value; },
                 {{ErrorCode::NonFiniteCoordinate, ObjectKind::Node, 0, {}, "y_m"}});
        badGraph("nonfinite geometry x " + suffix, [=](auto& g) { g.edges[0].geometry[1].x_m = value; },
                 {{ErrorCode::NonFiniteCoordinate, ObjectKind::Edge, 0, 1, "x_m"}});
        badTrajectory("nonfinite position x " + suffix, [=](auto& t) { t.points[1].position.x_m = value; },
                      {{ErrorCode::NonFiniteCoordinate, ObjectKind::TrajectoryPoint, 1, {}, "x_m"}});
    }
    test("invalid input retained without sorting or clamping", [] {
        auto input = trajectory();
        input.points[1].unix_time_ms = 0;
        input.points[0].accuracy_m = -2;
        const auto p = publishTrajectory(input);
        expectErrors(p.validation,
                     {{ErrorCode::NegativeValue, ObjectKind::TrajectoryPoint, 0, {}, "accuracy_m"},
                      {ErrorCode::TimestampDecreased, ObjectKind::TrajectoryPoint, 1, {}, "unix_time_ms"}});
        require(!p.snapshot && input.points[0].original_index == 7 &&
                input.points[1].unix_time_ms == 0 && input.points[0].accuracy_m == -2,
                "invalid input silently repaired");
    });
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
