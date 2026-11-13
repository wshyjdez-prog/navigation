#pragma once
#include "data/osmx/OsmxDataset.h"

namespace p1::app {
// Route modes offered on the map menu.
//  General: every routable way (current behaviour; footways, steps... included).
//  Driving: only ways a car may use, following Navit 0.5.7's "car" vehicle
//           profile (navit.xml roadprofile item types) and its OSM mapping in
//           maptool/osm.c. Pedestrian streets, footways, paths, cycleways,
//           steps, bridleways, construction and unmapped types are excluded;
//           motorcar / motor_vehicle / vehicle = no|private|agricultural|
//           forestry|delivery|destination remove car access (a more specific
//           "yes" overrides a general access=no, as in Navit).
enum class RouteMode { General, Driving };
QString routeModeName(RouteMode mode);
// True when a car may use the way under the Navit-derived rules above.
bool carAllowed(const data::OsmxDataset& roads, quint32 wayIndex);
}
