#include "RoutingProfile.h"

namespace p1::app {
namespace {
using data::RoadClass;
// Navit car profile item types and the OSM highway values maptool maps onto
// them: highway_*/street_n_lanes/street_1..4_*/ramp/living_street/
// street_service/street_parking_lane/track_paved|gravelled|unpaved/ferry.
bool carClass(RoadClass c) {
    switch(c) {
    case RoadClass::Motorway: case RoadClass::MotorwayLink:
    case RoadClass::Trunk: case RoadClass::TrunkLink:
    case RoadClass::Primary: case RoadClass::PrimaryLink:
    case RoadClass::Secondary: case RoadClass::SecondaryLink:
    case RoadClass::Tertiary: case RoadClass::TertiaryLink:
    case RoadClass::Unclassified: case RoadClass::Residential:
    case RoadClass::LivingStreet: case RoadClass::Service:
    case RoadClass::Track: case RoadClass::Ferry:
        return true;
    default:
        return false;
    }
}
// Navit access_value(): 1 = allowed, 2 = denied, 3 = other/unknown.
int accessValue(const QString& v) {
    if(v=="yes"||v=="1"||v=="designated"||v=="official"||v=="permissive") return 1;
    if(v=="no"||v=="0"||v=="private"||v=="agricultural"||v=="forestry"||v=="delivery"||v=="destination") return 2;
    return 3;
}
}
QString routeModeName(RouteMode mode) { return mode==RouteMode::Driving?QStringLiteral("行车模式"):QStringLiteral("一般模式"); }

bool carAllowed(const data::OsmxDataset& roads, quint32 wayIndex) {
    const auto& way=roads.wayAt(wayIndex);
    if(!way.attributes.isRoutable()||!carClass(way.attributes.roadClass)) return false;
    QString access,tracktype,surface;int specific=3;bool specificDenied=false;
    for(const auto& t:roads.wayTags(wayIndex)) {
        const auto key=roads.stringAt(t.key);const auto value=roads.stringAt(t.value).trimmed().toLower();
        if(key=="access") access=value;
        else if(key=="vehicle"||key=="motor_vehicle"||key=="motorcar") {
            const int a=accessValue(value);
            if(a==2) specificDenied=true; else if(a==1) specific=1;
        }
        else if(key=="tracktype") tracktype=value;
        else if(key=="surface") surface=value;
    }
    // Navit: ((default & ~generalDenied) | specificAllowed) & ~specificDenied.
    if(specificDenied) return false;
    if(accessValue(access)==2&&access!="destination"&&specific!=1) return false;
    // track_ground / track_grass are not in the car profile.
    if(way.attributes.roadClass==RoadClass::Track&&(tracktype=="grade4"||tracktype=="grade5"||surface=="ground"||surface=="grass")) return false;
    return true;
}
}
