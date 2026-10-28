#include "model/Validation.h"

#include <cmath>
#include <unordered_map>

namespace p1::algorithms::model {
namespace {

void error(ValidationResult& result, ErrorCode code, ObjectKind object,
           std::optional<std::size_t> index, const char* field,
           std::optional<std::size_t> point = std::nullopt) {
    result.errors.push_back({code, object, index, point, field});
}

bool finite(MetricPoint p) {
    return std::isfinite(p.x_m) && std::isfinite(p.y_m);
}

void coordinates(ValidationResult& result, MetricPoint p, ObjectKind object,
                 std::size_t index, std::optional<std::size_t> point = std::nullopt) {
    if (!std::isfinite(p.x_m))
        error(result, ErrorCode::NonFiniteCoordinate, object, index, "x_m", point);
    if (!std::isfinite(p.y_m))
        error(result, ErrorCode::NonFiniteCoordinate, object, index, "y_m", point);
}

void nonnegative(ValidationResult& result, double value, ObjectKind object,
                 std::size_t index, const char* field) {
    if (!std::isfinite(value))
        error(result, ErrorCode::NonFiniteValue, object, index, field);
    else if (value < 0.0)
        error(result, ErrorCode::NegativeValue, object, index, field);
}

bool blank(const std::string& value) {
    return value.find_first_not_of(" \t\r\n\f\v") == std::string::npos;
}

void metadata(ValidationResult& result, const DatasetMetadata& m) {
    if (blank(m.dataset_id))
        error(result, ErrorCode::MissingMetadata, ObjectKind::Metadata, {}, "dataset_id");
    if (blank(m.version))
        error(result, ErrorCode::MissingMetadata, ObjectKind::Metadata, {}, "version");
    if (blank(m.source))
        error(result, ErrorCode::MissingMetadata, ObjectKind::Metadata, {}, "source");
    if (blank(m.frame.definition))
        error(result, ErrorCode::MissingMetadata, ObjectKind::Metadata, {}, "frame.definition");
    if (m.frame.system != CoordinateSystem::LocalCartesian &&
        m.frame.system != CoordinateSystem::ProjectedCartesian)
        error(result, ErrorCode::UnsupportedCoordinateSystem, ObjectKind::Metadata, {}, "frame.system");
    if (m.frame.unit != LengthUnit::Metres)
        error(result, ErrorCode::UnsupportedLengthUnit, ObjectKind::Metadata, {}, "frame.unit");
}

void endpoint(ValidationResult& result, MetricPoint geometry, const Node& node,
              std::size_t index, std::size_t point, const char* field) {
    if (finite(geometry) && finite(node.position) &&
        std::hypot(geometry.x_m - node.position.x_m,
                   geometry.y_m - node.position.y_m) > endpoint_tolerance_m)
        error(result, ErrorCode::EndpointMismatch, ObjectKind::Edge, index, field, point);
}

} // namespace

ValidationResult validateGraph(const GraphData& input) {
    ValidationResult result;
    metadata(result, input.metadata);
    if (input.nodes.empty())
        error(result, ErrorCode::EmptyGraph, ObjectKind::Graph, {}, "nodes");

    std::unordered_map<std::uint64_t, NodeIndex> nodes;
    for (std::size_t i = 0; i < input.nodes.size(); ++i) {
        const auto& node = input.nodes[i];
        if (!nodes.emplace(node.id.value, NodeIndex{i}).second)
            error(result, ErrorCode::DuplicateNodeId, ObjectKind::Node, i, "id");
        coordinates(result, node.position, ObjectKind::Node, i);
    }

    std::unordered_map<std::uint64_t, EdgeIndex> edges;
    for (std::size_t i = 0; i < input.edges.size(); ++i) {
        const auto& edge = input.edges[i];
        if (!edges.emplace(edge.id.value, EdgeIndex{i}).second)
            error(result, ErrorCode::DuplicateEdgeId, ObjectKind::Edge, i, "id");
        const auto from = nodes.find(edge.from.value);
        const auto to = nodes.find(edge.to.value);
        if (from == nodes.end())
            error(result, ErrorCode::MissingEndpoint, ObjectKind::Edge, i, "from");
        if (to == nodes.end())
            error(result, ErrorCode::MissingEndpoint, ObjectKind::Edge, i, "to");
        nonnegative(result, edge.length_m, ObjectKind::Edge, i, "length_m");
        if (edge.length_m == 0.0)
            error(result, ErrorCode::ZeroLengthEdge, ObjectKind::Edge, i, "length_m");
        nonnegative(result, edge.cost_m, ObjectKind::Edge, i, "cost_m");

        if (edge.geometry.size() < 2)
            error(result, ErrorCode::TooFewGeometryPoints, ObjectKind::Edge, i, "geometry");
        bool all_finite = true;
        bool has_distinct_segment = false;
        for (std::size_t p = 0; p < edge.geometry.size(); ++p) {
            const auto current = edge.geometry[p];
            coordinates(result, current, ObjectKind::Edge, i, p);
            all_finite = all_finite && finite(current);
            if (p > 0 && finite(current) && finite(edge.geometry[p - 1])) {
                const auto previous = edge.geometry[p - 1];
                has_distinct_segment = has_distinct_segment ||
                    current.x_m != previous.x_m || current.y_m != previous.y_m;
            }
        }
        if (edge.geometry.size() >= 2 && all_finite && !has_distinct_segment)
            error(result, ErrorCode::DegenerateGeometry, ObjectKind::Edge, i, "geometry");
        if (!edge.geometry.empty()) {
            if (from != nodes.end())
                endpoint(result, edge.geometry.front(), input.nodes[from->second.value], i, 0, "from");
            if (to != nodes.end())
                endpoint(result, edge.geometry.back(), input.nodes[to->second.value], i,
                         edge.geometry.size() - 1, "to");
        }
    }
    return result;
}

ValidationResult validateTrajectory(const TrajectoryData& input) {
    ValidationResult result;
    metadata(result, input.metadata);
    if (input.points.empty())
        error(result, ErrorCode::EmptyTrajectory, ObjectKind::Trajectory, {}, "points");
    for (std::size_t i = 0; i < input.points.size(); ++i) {
        const auto& point = input.points[i];
        coordinates(result, point.position, ObjectKind::TrajectoryPoint, i);
        if (point.accuracy_m)
            nonnegative(result, *point.accuracy_m, ObjectKind::TrajectoryPoint, i, "accuracy_m");
        if (i > 0) {
            if (point.original_index <= input.points[i - 1].original_index)
                error(result, ErrorCode::OriginalIndexNotIncreasing, ObjectKind::TrajectoryPoint, i, "original_index");
            if (point.unix_time_ms < input.points[i - 1].unix_time_ms)
                error(result, ErrorCode::TimestampDecreased, ObjectKind::TrajectoryPoint, i, "unix_time_ms");
        }
    }
    return result;
}

GraphPublication publishGraph(const GraphData& input) {
    GraphData owned = input;
    auto validation = validateGraph(owned);
    if (!validation.ok()) return {nullptr, std::move(validation)};
    return {std::shared_ptr<const GraphSnapshot>(new const GraphSnapshot(std::move(owned))),
            std::move(validation)};
}

TrajectoryPublication publishTrajectory(const TrajectoryData& input) {
    TrajectoryData owned = input;
    auto validation = validateTrajectory(owned);
    if (!validation.ok()) return {nullptr, std::move(validation)};
    return {std::shared_ptr<const TrajectorySnapshot>(new const TrajectorySnapshot(std::move(owned))),
            std::move(validation)};
}

} // namespace p1::algorithms::model
