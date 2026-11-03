#pragma once

#include "OsmxTypes.h"

#include <QHash>
#include <QPair>
#include <QStringList>
#include <QVector>

#include <memory>
#include <optional>

namespace p1::data {

// On-disk entities keep their original OSM identifiers; array indices are the
// compact internal handles. Consumers must use IDs for lookup and indices for
// iteration/topology so a dataset can be shared without exposing layout.
struct OsmxNode {
    qint64 id = 0;
    double lat = 0.0;
    double lon = 0.0;
    quint32 tagFirst = 0;
    quint32 tagCount = 0;
};

struct OsmxWay {
    qint64 id = 0;
    quint32 refFirst = 0;  // index into OsmxDataset::wayNodePool()
    quint32 refCount = 0;
    quint32 tagFirst = 0;  // index into OsmxDataset::wayTagPool()
    quint32 tagCount = 0;
    double minLat = 0.0;
    double minLon = 0.0;
    double maxLat = 0.0;
    double maxLon = 0.0;
    bool hasGeometry = false;
    RoadAttributes attributes;
};

struct OsmxRelation {
    qint64 id = 0;
    quint32 memberFirst = 0; // index into OsmxDataset::memberPool()
    quint32 memberCount = 0;
    quint32 tagFirst = 0;
    quint32 tagCount = 0;
};

struct OsmxMember {
    OsmxMemberType type = OsmxMemberType::Node;
    qint64 ref = 0;
    quint32 role = kOsmxNoString;
};

// One directed movement along one way segment. A two-way segment produces two
// edges with the same way/segment but opposite from/to. Edge length is in
// metres and is the sum of nothing but that segment's great-circle distance.
struct OsmxEdge {
    quint32 from = 0;   // node index
    quint32 to = 0;     // node index
    quint32 way = 0;    // way index
    quint32 segment = 0; // 0 = between way refs [0] and [1]
    float lengthMeters = 0.0f;
};

// Everything a consumer needs before touching the arrays. Values are copied
// out of the file header / computed during load; none are estimates.
struct OsmxDatasetInfo {
    QString datasetId;
    QString sourceFileName;
    QString sourcePath;       // absolute path of the imported file
    QString headerSourcePath; // informational path stored inside the OSMX header
    QString sourceSha256;
    QString importedAt;       // ISO-8601 UTC
    quint32 formatVersion = 0;
    qint64 fileBytes = 0;
    OsmxBounds bounds;
    quint64 nodeTagCount = 0;
    quint64 wayTagCount = 0;
    quint64 relationTagCount = 0;
    quint64 wayNodeReferenceCount = 0;
    quint64 relationMemberCount = 0;
    quint32 routableWayCount = 0;
    quint32 undirectedSegmentCount = 0;
    quint32 directedEdgeCount = 0;
    double roadLengthMeters = 0.0;
    quint32 stringCount = 0;
    QStringList warnings;
};

// Immutable, thread-safe (read-only) road network loaded from one OSMX file.
// The object is created by OsmxParser/OsmxDataset::create, shared as
// shared_ptr<const OsmxDataset> and never mutated afterwards. Coordinates are
// WGS84 degrees; lengths are metres; IDs are the signed 64-bit OSM IDs.
class OsmxDataset final {
public:
    // Builder input. It exists for OsmxParser and unit tests only; callers who
    // want a dataset from a file must use OsmxParser so validation runs.
    struct Data {
        OsmxDatasetInfo info;
        QVector<OsmxNode> nodes;
        QVector<OsmxWay> ways;
        QVector<OsmxRelation> relations;
        QVector<OsmxTag> nodeTags;
        QVector<OsmxTag> wayTags;
        QVector<OsmxTag> relationTags;
        QVector<quint32> wayNodes; // node indices, in way order
        QVector<OsmxMember> members;
        QStringList strings;
        bool buildSpatialIndex = true;
    };

    // Builds lookup maps, node->way incidence, directed edges and the optional
    // node grid. Returns nullptr and fills *error on duplicate IDs or invalid
    // references. This never throws for malformed input; allocation failure
    // remains a std::bad_alloc.
    static std::shared_ptr<const OsmxDataset> create(Data&& data, OsmxError* error = nullptr);

    const OsmxDatasetInfo& info() const { return info_; }
    const OsmxBounds& bounds() const { return info_.bounds; }

    quint32 nodeCount() const { return static_cast<quint32>(nodes_.size()); }
    quint32 wayCount() const { return static_cast<quint32>(ways_.size()); }
    quint32 relationCount() const { return static_cast<quint32>(relations_.size()); }

    // Index access fails only for out-of-range indices; use the has* helpers
    // if the index may come from an untrusted source.
    const OsmxNode& nodeAt(quint32 index) const { return nodes_.at(static_cast<int>(index)); }
    const OsmxWay& wayAt(quint32 index) const { return ways_.at(static_cast<int>(index)); }
    const OsmxRelation& relationAt(quint32 index) const { return relations_.at(static_cast<int>(index)); }
    const OsmxEdge& edgeAt(quint32 index) const { return edges_.at(static_cast<int>(index)); }
    bool hasNode(quint32 index) const { return index < static_cast<quint32>(nodes_.size()); }
    bool hasWay(quint32 index) const { return index < static_cast<quint32>(ways_.size()); }
    bool hasRelation(quint32 index) const { return index < static_cast<quint32>(relations_.size()); }

    std::optional<quint32> nodeIndex(qint64 id) const;
    std::optional<quint32> wayIndex(qint64 id) const;
    std::optional<quint32> relationIndex(qint64 id) const;

    // Raw pools. Slicing with entity refFirst/refCount avoids per-query copies;
    // the convenience accessors below copy for callers that prefer values.
    const QVector<quint32>& wayNodePool() const { return wayNodes_; }
    const QVector<OsmxTag>& nodeTagPool() const { return nodeTags_; }
    const QVector<OsmxTag>& wayTagPool() const { return wayTags_; }
    const QVector<OsmxTag>& relationTagPool() const { return relationTags_; }
    const QVector<OsmxMember>& memberPool() const { return members_; }
    const QStringList& strings() const { return strings_; }

    QString stringAt(quint32 index) const;
    QStringView stringView(quint32 index) const;

    QVector<quint32> wayNodeIndices(quint32 wayIndex) const;
    QVector<qint64> wayNodeIds(quint32 wayIndex) const;

    QVector<OsmxTag> nodeTags(quint32 nodeIndex) const;
    QVector<OsmxTag> wayTags(quint32 wayIndex) const;
    QVector<OsmxTag> relationTags(quint32 relationIndex) const;
    QVector<OsmxMember> relationMembers(quint32 relationIndex) const;

    // Tag lookups compare the interned key. An absent tag returns nullopt; an
    // empty value returns an empty QString, so callers can distinguish them.
    std::optional<QString> nodeTag(quint32 nodeIndex, QStringView key) const;
    std::optional<QString> wayTag(quint32 wayIndex, QStringView key) const;
    std::optional<QString> relationTag(quint32 relationIndex, QStringView key) const;
    bool nodeHasTag(quint32 nodeIndex, QStringView key) const;
    bool wayHasTag(quint32 wayIndex, QStringView key) const;

    QString displayName(quint32 wayIndex) const; // empty when untagged

    // Node -> way incidence (ways that reference the node), deduplicated by way
    // index but preserving way order. Useful for rendering junctions and for
    // building turn restrictions; A* itself should iterate edges.
    quint32 incidentWayFirst(quint32 nodeIndex) const;
    quint32 incidentWayCount(quint32 nodeIndex) const;
    QVector<quint32> incidentWays(quint32 nodeIndex) const;

    // Directed edges grouped by from-node. The range is valid for node indices
    // [0, nodeCount()). Empty for isolated nodes.
    quint32 outgoingEdgeFirst(quint32 nodeIndex) const;
    quint32 outgoingEdgeCount(quint32 nodeIndex) const;
    const QVector<OsmxEdge>& edges() const { return edges_; }

    // Spatial helpers: nearest node within maxDistanceMeters (nullopt when the
    // grid is disabled or nothing is close enough) and every way whose bounds
    // intersect the query, optionally limited to routable ways.
    //
    // nearestNode() searches the node grid cell by cell; the search radius is
    // capped at kNearestNodeMaxGridRadius cells (~35 km with the 0.005° grid),
    // so a larger maxDistanceMeters silently behaves as that cap. Use it to snap
    // a GPS fix to the network, not as a global nearest search.
    static constexpr int kNearestNodeMaxGridRadius = 64;
    std::optional<quint32> nearestNode(double lat, double lon, double maxDistanceMeters = 500.0,
                                       double* distanceMeters = nullptr) const;
    QVector<quint32> waysInBounds(const OsmxBounds& area, int limit = 0,
                                  bool routableOnly = false) const;
    bool hasSpatialIndex() const { return !grid_.isEmpty(); }

private:
    OsmxDataset() = default;

    OsmxDatasetInfo info_;
    QVector<OsmxNode> nodes_;
    QVector<OsmxWay> ways_;
    QVector<OsmxRelation> relations_;
    QVector<OsmxTag> nodeTags_;
    QVector<OsmxTag> wayTags_;
    QVector<OsmxTag> relationTags_;
    QVector<quint32> wayNodes_;
    QVector<OsmxMember> members_;
    QStringList strings_;
    QHash<qint64, quint32> nodeIndex_;
    QHash<qint64, quint32> wayIndex_;
    QHash<qint64, quint32> relationIndex_;
    QVector<quint32> incidentOffsets_;
    QVector<quint32> incidentWays_;
    QVector<quint32> outgoingOffsets_;
    QVector<OsmxEdge> edges_;
    QHash<quint64, QVector<quint32>> grid_;
    double gridOriginLat_ = 0.0;
    double gridOriginLon_ = 0.0;
    double gridCellDegrees_ = 0.005;
};

} // namespace p1::data
