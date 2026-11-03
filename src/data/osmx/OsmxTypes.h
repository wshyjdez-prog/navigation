#pragma once

#include <QString>
#include <QStringView>
#include <QtGlobal>

namespace p1::data {

// OSMX is the cleaned, fixed-width road-network container used by P1. The
// reader is versioned: bump kOsmxVersion when the byte layout or field meaning
// changes, never silently reuse a version for a different schema.
constexpr quint32 kOsmxVersion = 1;
constexpr quint32 kOsmxNoString = 0xFFFFFFFFu;
constexpr quint64 kOsmxMaxFileBytes = 8ull * 1024ull * 1024ull * 1024ull;
constexpr quint32 kOsmxMaxStringBytes = 1024u * 1024u;
constexpr double kOsmxEarthRadiusMeters = 6371008.8;

// Error categories are stable for callers and tests; the human readable text
// carries file offsets but is not part of the contract.
enum class OsmxErrorCode : quint8 {
    None = 0,
    OpenFailed,
    IoError,
    FileTooLarge,
    Truncated,
    BadMagic,
    UnsupportedVersion,
    UnsupportedFlags,
    BadCount,
    BadString,
    BadCoordinate,
    BadBounds,
    DuplicateId,
    CorruptRecord,
    TrailingData,
    FileChanged,
    Cancelled,
    InvalidDataset
};

QString osmxErrorCodeName(OsmxErrorCode code);

struct OsmxError {
    OsmxErrorCode code = OsmxErrorCode::None;
    QString message;
    qint64 offset = -1;

    bool isOk() const { return code == OsmxErrorCode::None; }
    void clear() { code = OsmxErrorCode::None; message.clear(); offset = -1; }
    QString toString() const;
};

// WGS84 degrees. valid=false means the file did not declare bounds; the
// dataset then publishes bounds computed from the node table.
struct OsmxBounds {
    double minLat = 0.0;
    double minLon = 0.0;
    double maxLat = 0.0;
    double maxLon = 0.0;
    bool valid = false;

    bool contains(double lat, double lon) const {
        return valid && lat >= minLat && lat <= maxLat && lon >= minLon && lon <= maxLon;
    }
    bool intersects(const OsmxBounds& other) const {
        return valid && other.valid && minLat <= other.maxLat && maxLat >= other.minLat &&
               minLon <= other.maxLon && maxLon >= other.minLon;
    }
    double centerLat() const { return (minLat + maxLat) * 0.5; }
    double centerLon() const { return (minLon + maxLon) * 0.5; }
};

// Highway values are kept as a closed enum so algorithms can switch on a
// stable class instead of comparing raw strings. Unknown values are preserved
// as RoadClass::Unknown and remain queryable through the raw tag.
enum class RoadClass : quint8 {
    Unknown = 0,
    Motorway,
    MotorwayLink,
    Trunk,
    TrunkLink,
    Primary,
    PrimaryLink,
    Secondary,
    SecondaryLink,
    Tertiary,
    TertiaryLink,
    Unclassified,
    Residential,
    LivingStreet,
    Service,
    Pedestrian,
    Track,
    Footway,
    Cycleway,
    Path,
    Steps,
    Bridleway,
    Construction,
    Busway,
    BusGuideway,
    Escape,
    Corridor,
    Platform,
    Raceway,
    Rail,
    Ferry
};

RoadClass roadClassFromHighway(QStringView highway);
QString roadClassName(RoadClass roadClass);
bool roadClassIsRoutable(RoadClass roadClass);

enum class OsmxMemberType : quint8 { Node = 0, Way = 1, Relation = 2 };
QString osmxMemberTypeName(OsmxMemberType type);

// Tag keys and values are interned in the dataset string pool. A tag is a pair
// of pool indices, so tag lists stay disengaged from task- or locale-specific
// QString allocation.
struct OsmxTag {
    quint32 key = kOsmxNoString;
    quint32 value = kOsmxNoString;
};

// Bit flags on RoadAttributes. Only the low 8 bits are stored on disk-adjacent
// structures; new flags must be added here with a documented meaning.
enum OsmxRoadFlag : quint8 {
    RoadBridge = 1u << 0,
    RoadTunnel = 1u << 1,
    RoadOneway = 1u << 2,
    RoadOnewayReversed = 1u << 3,
    RoadRoutable = 1u << 4,
    RoadAccessDenied = 1u << 5,
    RoadArea = 1u << 6,
    RoadIncompleteGeometry = 1u << 7
};

// Precomputed per-way attributes shared by A*/HMM (3) and rendering (5).
// lengthMeters is the sum of great-circle segment lengths, never an estimate.
struct RoadAttributes {
    RoadClass roadClass = RoadClass::Unknown;
    quint8 flags = 0;
    quint8 lanes = 0;       // 0 = not tagged
    quint16 maxSpeedKmh = 0; // 0 = not tagged / unparsable
    qint16 layer = 0;
    float lengthMeters = 0.0f;
    quint32 name = kOsmxNoString; // display name (name:zh > name > name:en)

    bool hasFlag(OsmxRoadFlag flag) const { return (flags & static_cast<quint8>(flag)) != 0; }
    bool isRoutable() const { return hasFlag(RoadRoutable); }
    bool isOneway() const { return hasFlag(RoadOneway); }
    bool isOnewayReversed() const { return hasFlag(RoadOnewayReversed); }
    bool isBridge() const { return hasFlag(RoadBridge); }
    bool isTunnel() const { return hasFlag(RoadTunnel); }
    bool isArea() const { return hasFlag(RoadArea); }
};

// Great-circle distance in metres (haversine, mean Earth radius). Exposed so
// every consumer measures geometry with the same formula and units.
double osmxDistanceMeters(double lat1, double lon1, double lat2, double lon2);

} // namespace p1::data
