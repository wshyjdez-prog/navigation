#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace p1::algorithms::model {

// IDs are dataset identifiers; indices address an input vector, never an ID.
template <class Tag, class Value>
struct Identifier {
    Value value{};
    explicit constexpr Identifier(Value v = {}) noexcept : value(v) {}
    friend constexpr bool operator==(Identifier a, Identifier b) noexcept {
        return a.value == b.value;
    }
    friend constexpr bool operator!=(Identifier a, Identifier b) noexcept {
        return !(a == b);
    }
};

using NodeId = Identifier<struct NodeIdTag, std::uint64_t>;
using EdgeId = Identifier<struct EdgeIdTag, std::uint64_t>;
using NodeIndex = Identifier<struct NodeIndexTag, std::size_t>;
using EdgeIndex = Identifier<struct EdgeIndexTag, std::size_t>;

enum class CoordinateSystem { LocalCartesian, ProjectedCartesian };
enum class LengthUnit { Metres };

struct CoordinateFrame {
    CoordinateSystem system{CoordinateSystem::LocalCartesian};
    LengthUnit unit{LengthUnit::Metres};
    // Local: origin/axes definition. Projected: CRS and transform description.
    std::string definition;
};

struct DatasetMetadata {
    std::string dataset_id;
    std::string version; // Opaque, nonblank revision; not a sortable number.
    std::string source;
    CoordinateFrame frame;
};

struct MetricPoint {
    double x_m{};
    double y_m{};
};

struct Node {
    NodeId id;
    MetricPoint position;
};

struct DirectedEdge {
    EdgeId id;
    NodeId from;
    NodeId to;
    std::vector<MetricPoint> geometry; // from -> to; reverse travel needs another edge.
    double length_m{}; // Supplied positive length; exact computation is P1.
    double cost_m{};   // Nonnegative distance cost; zero is permitted.
};

struct GraphData {
    DatasetMetadata metadata;
    std::vector<Node> nodes;
    std::vector<DirectedEdge> edges;
};

struct TrajectoryPoint {
    std::uint64_t original_index{}; // Strictly increasing; gaps are permitted.
    MetricPoint position;
    std::int64_t unix_time_ms{}; // Nondecreasing; equal times are retained.
    std::optional<double> accuracy_m; // Device accuracy, not a Gaussian sigma.
};

struct TrajectoryData {
    DatasetMetadata metadata;
    std::vector<TrajectoryPoint> points;
};

struct GraphPublication;
struct TrajectoryPublication;

class GraphSnapshot final {
public:
    const GraphData& data() const noexcept { return data_; }

private:
    explicit GraphSnapshot(GraphData&& owned) : data_(std::move(owned)) {}
    const GraphData data_;
    friend GraphPublication publishGraph(const GraphData& input);
};

class TrajectorySnapshot final {
public:
    const TrajectoryData& data() const noexcept { return data_; }

private:
    explicit TrajectorySnapshot(TrajectoryData&& owned) : data_(std::move(owned)) {}
    const TrajectoryData data_;
    friend TrajectoryPublication publishTrajectory(const TrajectoryData& input);
};

} // namespace p1::algorithms::model
