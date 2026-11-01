#pragma once
#include "model/Types.h"

namespace p1::algorithms::samples {
// Synthetic metre coordinates. These are not WGS84 coordinates or imported roads.
inline model::GraphData fixedRoadNetwork() {
    using namespace model;
    return {{"member3-astar-synthetic", "v1", "hand-written deterministic fixture",
             {CoordinateSystem::LocalCartesian, LengthUnit::Metres,
              "synthetic origin (0,0); x east and y north in metres"}},
            {{NodeId{900}, {0, 0}}, {NodeId{42}, {3, 0}}, {NodeId{701}, {0, 5}},
             {NodeId{5}, {6, 0}}, {NodeId{777}, {99, 99}}},
            {{EdgeId{10}, NodeId{900}, NodeId{42}, {{0, 0}, {3, 0}}, 3, 3},
             {EdgeId{11}, NodeId{42}, NodeId{5}, {{3, 0}, {6, 0}}, 3, 3},
             {EdgeId{12}, NodeId{900}, NodeId{701}, {{0, 0}, {0, 5}}, 5, 5},
             {EdgeId{13}, NodeId{701}, NodeId{5}, {{0, 5}, {6, 0}}, 10, 5},
             {EdgeId{14}, NodeId{900}, NodeId{5}, {{0, 0}, {6, 0}}, 6, 9},
             {EdgeId{15}, NodeId{900}, NodeId{42}, {{0, 0}, {3, 0}}, 100, 20}}};
}
} // namespace p1::algorithms::samples
