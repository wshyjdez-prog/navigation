#include "OsmxTypes.h"

#include <QLatin1String>

#include <algorithm>
#include <cmath>

namespace p1::data {
namespace {

constexpr double kPi = 3.14159265358979323846;

bool isTag(QStringView value, QLatin1String expected) {
    return value.compare(expected, Qt::CaseInsensitive) == 0;
}

} // namespace

QString osmxErrorCodeName(OsmxErrorCode code) {
    switch (code) {
    case OsmxErrorCode::None: return QStringLiteral("ok");
    case OsmxErrorCode::OpenFailed: return QStringLiteral("open_failed");
    case OsmxErrorCode::IoError: return QStringLiteral("io_error");
    case OsmxErrorCode::FileTooLarge: return QStringLiteral("file_too_large");
    case OsmxErrorCode::Truncated: return QStringLiteral("truncated");
    case OsmxErrorCode::BadMagic: return QStringLiteral("bad_magic");
    case OsmxErrorCode::UnsupportedVersion: return QStringLiteral("unsupported_version");
    case OsmxErrorCode::UnsupportedFlags: return QStringLiteral("unsupported_flags");
    case OsmxErrorCode::BadCount: return QStringLiteral("bad_count");
    case OsmxErrorCode::BadString: return QStringLiteral("bad_string");
    case OsmxErrorCode::BadCoordinate: return QStringLiteral("bad_coordinate");
    case OsmxErrorCode::BadBounds: return QStringLiteral("bad_bounds");
    case OsmxErrorCode::DuplicateId: return QStringLiteral("duplicate_id");
    case OsmxErrorCode::CorruptRecord: return QStringLiteral("corrupt_record");
    case OsmxErrorCode::TrailingData: return QStringLiteral("trailing_data");
    case OsmxErrorCode::FileChanged: return QStringLiteral("file_changed");
    case OsmxErrorCode::Cancelled: return QStringLiteral("cancelled");
    case OsmxErrorCode::InvalidDataset: return QStringLiteral("invalid_dataset");
    }
    return QStringLiteral("unknown");
}

QString OsmxError::toString() const {
    if (isOk()) return QStringLiteral("ok");
    QString text = QStringLiteral("%1: %2").arg(osmxErrorCodeName(code), message);
    if (offset >= 0) text += QStringLiteral("（文件偏移 %1）").arg(offset);
    return text;
}

RoadClass roadClassFromHighway(QStringView highway) {
    if (isTag(highway, QLatin1String("motorway"))) return RoadClass::Motorway;
    if (isTag(highway, QLatin1String("motorway_link"))) return RoadClass::MotorwayLink;
    if (isTag(highway, QLatin1String("trunk"))) return RoadClass::Trunk;
    if (isTag(highway, QLatin1String("trunk_link"))) return RoadClass::TrunkLink;
    if (isTag(highway, QLatin1String("primary"))) return RoadClass::Primary;
    if (isTag(highway, QLatin1String("primary_link"))) return RoadClass::PrimaryLink;
    if (isTag(highway, QLatin1String("secondary"))) return RoadClass::Secondary;
    if (isTag(highway, QLatin1String("secondary_link"))) return RoadClass::SecondaryLink;
    if (isTag(highway, QLatin1String("tertiary"))) return RoadClass::Tertiary;
    if (isTag(highway, QLatin1String("tertiary_link"))) return RoadClass::TertiaryLink;
    if (isTag(highway, QLatin1String("unclassified"))) return RoadClass::Unclassified;
    if (isTag(highway, QLatin1String("residential"))) return RoadClass::Residential;
    if (isTag(highway, QLatin1String("living_street"))) return RoadClass::LivingStreet;
    if (isTag(highway, QLatin1String("service"))) return RoadClass::Service;
    if (isTag(highway, QLatin1String("pedestrian"))) return RoadClass::Pedestrian;
    if (isTag(highway, QLatin1String("track"))) return RoadClass::Track;
    if (isTag(highway, QLatin1String("footway"))) return RoadClass::Footway;
    if (isTag(highway, QLatin1String("cycleway"))) return RoadClass::Cycleway;
    if (isTag(highway, QLatin1String("path"))) return RoadClass::Path;
    if (isTag(highway, QLatin1String("steps"))) return RoadClass::Steps;
    if (isTag(highway, QLatin1String("bridleway"))) return RoadClass::Bridleway;
    if (isTag(highway, QLatin1String("construction"))) return RoadClass::Construction;
    if (isTag(highway, QLatin1String("busway"))) return RoadClass::Busway;
    if (isTag(highway, QLatin1String("bus_guideway"))) return RoadClass::BusGuideway;
    if (isTag(highway, QLatin1String("escape"))) return RoadClass::Escape;
    if (isTag(highway, QLatin1String("corridor"))) return RoadClass::Corridor;
    if (isTag(highway, QLatin1String("platform"))) return RoadClass::Platform;
    if (isTag(highway, QLatin1String("raceway"))) return RoadClass::Raceway;
    if (isTag(highway, QLatin1String("rail"))) return RoadClass::Rail;
    if (isTag(highway, QLatin1String("ferry"))) return RoadClass::Ferry;
    return RoadClass::Unknown;
}

QString roadClassName(RoadClass roadClass) {
    switch (roadClass) {
    case RoadClass::Unknown: return QStringLiteral("unknown");
    case RoadClass::Motorway: return QStringLiteral("motorway");
    case RoadClass::MotorwayLink: return QStringLiteral("motorway_link");
    case RoadClass::Trunk: return QStringLiteral("trunk");
    case RoadClass::TrunkLink: return QStringLiteral("trunk_link");
    case RoadClass::Primary: return QStringLiteral("primary");
    case RoadClass::PrimaryLink: return QStringLiteral("primary_link");
    case RoadClass::Secondary: return QStringLiteral("secondary");
    case RoadClass::SecondaryLink: return QStringLiteral("secondary_link");
    case RoadClass::Tertiary: return QStringLiteral("tertiary");
    case RoadClass::TertiaryLink: return QStringLiteral("tertiary_link");
    case RoadClass::Unclassified: return QStringLiteral("unclassified");
    case RoadClass::Residential: return QStringLiteral("residential");
    case RoadClass::LivingStreet: return QStringLiteral("living_street");
    case RoadClass::Service: return QStringLiteral("service");
    case RoadClass::Pedestrian: return QStringLiteral("pedestrian");
    case RoadClass::Track: return QStringLiteral("track");
    case RoadClass::Footway: return QStringLiteral("footway");
    case RoadClass::Cycleway: return QStringLiteral("cycleway");
    case RoadClass::Path: return QStringLiteral("path");
    case RoadClass::Steps: return QStringLiteral("steps");
    case RoadClass::Bridleway: return QStringLiteral("bridleway");
    case RoadClass::Construction: return QStringLiteral("construction");
    case RoadClass::Busway: return QStringLiteral("busway");
    case RoadClass::BusGuideway: return QStringLiteral("bus_guideway");
    case RoadClass::Escape: return QStringLiteral("escape");
    case RoadClass::Corridor: return QStringLiteral("corridor");
    case RoadClass::Platform: return QStringLiteral("platform");
    case RoadClass::Raceway: return QStringLiteral("raceway");
    case RoadClass::Rail: return QStringLiteral("rail");
    case RoadClass::Ferry: return QStringLiteral("ferry");
    }
    return QStringLiteral("unknown");
}

bool roadClassIsRoutable(RoadClass roadClass) {
    switch (roadClass) {
    case RoadClass::Unknown:
    case RoadClass::Construction:
    case RoadClass::Raceway:
    case RoadClass::BusGuideway:
        return false;
    default:
        return true;
    }
}

QString osmxMemberTypeName(OsmxMemberType type) {
    switch (type) {
    case OsmxMemberType::Node: return QStringLiteral("node");
    case OsmxMemberType::Way: return QStringLiteral("way");
    case OsmxMemberType::Relation: return QStringLiteral("relation");
    }
    return QStringLiteral("unknown");
}

double osmxDistanceMeters(double lat1, double lon1, double lat2, double lon2) {
    const double dLat = (lat2 - lat1) * kPi / 180.0;
    const double dLon = (lon2 - lon1) * kPi / 180.0;
    const double a = std::sin(dLat * 0.5) * std::sin(dLat * 0.5) +
                     std::cos(lat1 * kPi / 180.0) * std::cos(lat2 * kPi / 180.0) *
                         std::sin(dLon * 0.5) * std::sin(dLon * 0.5);
    return 2.0 * kOsmxEarthRadiusMeters * std::asin(std::sqrt(std::min(1.0, a)));
}

} // namespace p1::data
