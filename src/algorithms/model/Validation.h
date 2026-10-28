#pragma once

#include "model/Types.h"

namespace p1::algorithms::model {

inline constexpr double endpoint_tolerance_m = 1e-6;

enum class ErrorCode {
    MissingMetadata,
    UnsupportedCoordinateSystem,
    UnsupportedLengthUnit,
    EmptyGraph,
    EmptyTrajectory,
    DuplicateNodeId,
    DuplicateEdgeId,
    MissingEndpoint,
    NonFiniteCoordinate,
    NonFiniteValue,
    NegativeValue,
    ZeroLengthEdge,
    TooFewGeometryPoints,
    DegenerateGeometry,
    EndpointMismatch,
    OriginalIndexNotIncreasing,
    TimestampDecreased
};

enum class ObjectKind { Metadata, Graph, Node, Edge, Trajectory, TrajectoryPoint };

struct ValidationError {
    ErrorCode code;
    ObjectKind object;
    // Zero-based input vector position, not an ID or original trajectory index.
    std::optional<std::size_t> index;
    std::optional<std::size_t> geometry_point_index;
    std::string field;
};

struct ValidationResult {
    std::vector<ValidationError> errors;
    bool ok() const noexcept { return errors.empty(); }
};

struct GraphPublication {
    std::shared_ptr<const GraphSnapshot> snapshot;
    ValidationResult validation;
};

struct TrajectoryPublication {
    std::shared_ptr<const TrajectorySnapshot> snapshot;
    ValidationResult validation;
};

ValidationResult validateGraph(const GraphData& input);
ValidationResult validateTrajectory(const TrajectoryData& input);

// Always deep-copy input (even an rvalue), validate that owned copy, then move
// only the private copy into an actually const snapshot. Invalid input yields
// no snapshot. Callers must not mutate input concurrently during these calls.
GraphPublication publishGraph(const GraphData& input);
TrajectoryPublication publishTrajectory(const TrajectoryData& input);

} // namespace p1::algorithms::model
