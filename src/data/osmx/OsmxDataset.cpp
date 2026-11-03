#include "OsmxDataset.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace p1::data {
namespace {

std::shared_ptr<const OsmxDataset> fail(OsmxError* error, OsmxErrorCode code, const QString& message) {
    if (error != nullptr) {
        error->code = code;
        error->message = message;
    }
    return nullptr;
}

bool validStringIndex(const QStringList& strings, quint32 index) {
    return index == kOsmxNoString || index < static_cast<quint32>(strings.size());
}

bool containsFinitePoint(double lat, double lon) {
    return std::isfinite(lat) && std::isfinite(lon);
}

std::optional<QString> findTag(const QVector<OsmxTag>& pool, const QStringList& strings, quint32 first,
                               quint32 count, QStringView key) {
    const quint32 end = first + count;
    if (end > static_cast<quint32>(pool.size())) return std::nullopt;
    for (quint32 i = first; i < end; ++i) {
        const OsmxTag& tag = pool.at(static_cast<int>(i));
        if (tag.key == kOsmxNoString) continue;
        if (strings.at(static_cast<int>(tag.key)) == key) {
            return tag.value == kOsmxNoString ? QString() : strings.at(static_cast<int>(tag.value));
        }
    }
    return std::nullopt;
}

} // namespace

std::shared_ptr<const OsmxDataset> OsmxDataset::create(Data&& data, OsmxError* error) {
    if (data.nodes.size() > static_cast<qsizetype>(std::numeric_limits<quint32>::max())) {
        return fail(error, OsmxErrorCode::BadCount, QStringLiteral("节点数量超过 32 位索引上限"));
    }
    if (data.ways.size() > static_cast<qsizetype>(std::numeric_limits<quint32>::max())) {
        return fail(error, OsmxErrorCode::BadCount, QStringLiteral("道路数量超过 32 位索引上限"));
    }
    if (data.strings.isEmpty()) data.strings.append(QString()); // index 0 is the empty string

    // Structural validation of slices. The parser already enforces these, but
    // the builder is also used by tests and must not create an unsafe dataset.
    for (const auto& node : data.nodes) {
        if (node.id == 0) return fail(error, OsmxErrorCode::InvalidDataset, QStringLiteral("节点 ID 不能为 0"));
        if (node.tagFirst + node.tagCount > static_cast<quint32>(data.nodeTags.size()))
            return fail(error, OsmxErrorCode::InvalidDataset, QStringLiteral("节点标签范围越界"));
        for (quint32 i = 0; i < node.tagCount; ++i) {
            const auto& tag = data.nodeTags.at(static_cast<int>(node.tagFirst + i));
            if (!validStringIndex(data.strings, tag.key) || !validStringIndex(data.strings, tag.value))
                return fail(error, OsmxErrorCode::InvalidDataset, QStringLiteral("节点标签字符串索引越界"));
        }
    }
    for (const auto& way : data.ways) {
        if (way.id == 0) return fail(error, OsmxErrorCode::InvalidDataset, QStringLiteral("道路 ID 不能为 0"));
        if (way.refFirst + way.refCount > static_cast<quint32>(data.wayNodes.size()))
            return fail(error, OsmxErrorCode::InvalidDataset, QStringLiteral("道路引用范围越界"));
        if (way.tagFirst + way.tagCount > static_cast<quint32>(data.wayTags.size()))
            return fail(error, OsmxErrorCode::InvalidDataset, QStringLiteral("道路标签范围越界"));
        for (quint32 i = 0; i < way.refCount; ++i) {
            const quint32 node = data.wayNodes.at(static_cast<int>(way.refFirst + i));
            if (node >= static_cast<quint32>(data.nodes.size()))
                return fail(error, OsmxErrorCode::InvalidDataset, QStringLiteral("道路引用了不存在的节点索引"));
        }
        for (quint32 i = 0; i < way.tagCount; ++i) {
            const auto& tag = data.wayTags.at(static_cast<int>(way.tagFirst + i));
            if (!validStringIndex(data.strings, tag.key) || !validStringIndex(data.strings, tag.value))
                return fail(error, OsmxErrorCode::InvalidDataset, QStringLiteral("道路标签字符串索引越界"));
        }
    }
    for (const auto& relation : data.relations) {
        if (relation.id == 0) return fail(error, OsmxErrorCode::InvalidDataset, QStringLiteral("关系 ID 不能为 0"));
        if (relation.memberFirst + relation.memberCount > static_cast<quint32>(data.members.size()))
            return fail(error, OsmxErrorCode::InvalidDataset, QStringLiteral("关系成员范围越界"));
        if (relation.tagFirst + relation.tagCount > static_cast<quint32>(data.relationTags.size()))
            return fail(error, OsmxErrorCode::InvalidDataset, QStringLiteral("关系标签范围越界"));
        for (quint32 i = 0; i < relation.memberCount; ++i) {
            const auto& member = data.members.at(static_cast<int>(relation.memberFirst + i));
            if (member.type != OsmxMemberType::Node && member.type != OsmxMemberType::Way &&
                member.type != OsmxMemberType::Relation)
                return fail(error, OsmxErrorCode::InvalidDataset, QStringLiteral("关系成员类型无效"));
            if (member.role != kOsmxNoString && member.role >= static_cast<quint32>(data.strings.size()))
                return fail(error, OsmxErrorCode::InvalidDataset, QStringLiteral("关系成员角色字符串索引越界"));
        }
        for (quint32 i = 0; i < relation.tagCount; ++i) {
            const auto& tag = data.relationTags.at(static_cast<int>(relation.tagFirst + i));
            if (!validStringIndex(data.strings, tag.key) || !validStringIndex(data.strings, tag.value))
                return fail(error, OsmxErrorCode::InvalidDataset, QStringLiteral("关系标签字符串索引越界"));
        }
    }

    auto dataset = std::shared_ptr<OsmxDataset>(new OsmxDataset());
    dataset->info_ = std::move(data.info);
    dataset->nodes_ = std::move(data.nodes);
    dataset->ways_ = std::move(data.ways);
    dataset->relations_ = std::move(data.relations);
    dataset->nodeTags_ = std::move(data.nodeTags);
    dataset->wayTags_ = std::move(data.wayTags);
    dataset->relationTags_ = std::move(data.relationTags);
    dataset->wayNodes_ = std::move(data.wayNodes);
    dataset->members_ = std::move(data.members);
    dataset->strings_ = std::move(data.strings);
    dataset->info_.stringCount = static_cast<quint32>(dataset->strings_.size());

    // ID maps also reject duplicate identifiers: a cleaned network must not
    // contain two objects with the same OSM ID.
    dataset->nodeIndex_.reserve(dataset->nodes_.size());
    for (quint32 i = 0; i < dataset->nodeCount(); ++i) {
        const qint64 id = dataset->nodes_.at(static_cast<int>(i)).id;
        if (dataset->nodeIndex_.contains(id))
            return fail(error, OsmxErrorCode::DuplicateId, QStringLiteral("节点 ID 重复：%1").arg(id));
        dataset->nodeIndex_.insert(id, i);
    }
    dataset->wayIndex_.reserve(dataset->ways_.size());
    for (quint32 i = 0; i < dataset->wayCount(); ++i) {
        const qint64 id = dataset->ways_.at(static_cast<int>(i)).id;
        if (dataset->wayIndex_.contains(id))
            return fail(error, OsmxErrorCode::DuplicateId, QStringLiteral("道路 ID 重复：%1").arg(id));
        dataset->wayIndex_.insert(id, i);
    }
    dataset->relationIndex_.reserve(dataset->relations_.size());
    for (quint32 i = 0; i < dataset->relationCount(); ++i) {
        const qint64 id = dataset->relations_.at(static_cast<int>(i)).id;
        if (dataset->relationIndex_.contains(id))
            return fail(error, OsmxErrorCode::DuplicateId, QStringLiteral("关系 ID 重复：%1").arg(id));
        dataset->relationIndex_.insert(id, i);
    }

    const quint32 nodeCount = dataset->nodeCount();
    const quint32 wayCount = dataset->wayCount();

    // Node -> way incidence. Counts include repeated references inside one way;
    // the per-node list is sorted/deduplicated below so callers see one entry.
    QVector<quint32> incidentCounts(nodeCount + 1, 0);
    for (const auto& way : dataset->ways_) {
        for (quint32 i = 0; i < way.refCount; ++i) {
            const quint32 node = dataset->wayNodes_.at(static_cast<int>(way.refFirst + i));
            ++incidentCounts[static_cast<int>(node + 1)];
        }
    }
    dataset->incidentOffsets_.resize(nodeCount + 1);
    dataset->incidentOffsets_[0] = 0;
    for (quint32 i = 0; i < nodeCount; ++i) {
        dataset->incidentOffsets_[static_cast<int>(i + 1)] =
            dataset->incidentOffsets_.at(static_cast<int>(i)) + incidentCounts.at(static_cast<int>(i + 1));
    }
    QVector<quint32> incidentCursor = dataset->incidentOffsets_;
    QVector<quint32> incidentRaw(static_cast<qsizetype>(dataset->incidentOffsets_.last()), 0);
    for (quint32 w = 0; w < wayCount; ++w) {
        const auto& way = dataset->ways_.at(static_cast<int>(w));
        for (quint32 i = 0; i < way.refCount; ++i) {
            const quint32 node = dataset->wayNodes_.at(static_cast<int>(way.refFirst + i));
            const quint32 slot = incidentCursor[static_cast<int>(node)]++;
            incidentRaw[static_cast<int>(slot)] = w;
        }
    }
    // Compact sorted/unique slices into the final CSR arrays.
    dataset->incidentWays_.clear();
    dataset->incidentWays_.reserve(incidentRaw.size());
    QVector<quint32> compactOffsets(nodeCount + 1, 0);
    for (quint32 node = 0; node < nodeCount; ++node) {
        compactOffsets[static_cast<int>(node)] = static_cast<quint32>(dataset->incidentWays_.size());
        const quint32 begin = dataset->incidentOffsets_.at(static_cast<int>(node));
        const quint32 end = dataset->incidentOffsets_.at(static_cast<int>(node + 1));
        if (begin == end) continue;
        std::sort(incidentRaw.begin() + begin, incidentRaw.begin() + end);
        quint32 previous = std::numeric_limits<quint32>::max();
        for (quint32 i = begin; i < end; ++i) {
            const quint32 way = incidentRaw.at(static_cast<int>(i));
            if (way == previous) continue;
            dataset->incidentWays_.append(way);
            previous = way;
        }
    }
    compactOffsets[static_cast<int>(nodeCount)] = static_cast<quint32>(dataset->incidentWays_.size());
    dataset->incidentOffsets_ = std::move(compactOffsets);

    // Way geometry, attributes and directed edges.
    OsmxBounds computed;
    for (const auto& node : dataset->nodes_) {
        if (!containsFinitePoint(node.lat, node.lon)) continue;
        if (!computed.valid) {
            computed = OsmxBounds{node.lat, node.lon, node.lat, node.lon, true};
        } else {
            computed.minLat = std::min(computed.minLat, node.lat);
            computed.minLon = std::min(computed.minLon, node.lon);
            computed.maxLat = std::max(computed.maxLat, node.lat);
            computed.maxLon = std::max(computed.maxLon, node.lon);
        }
    }
    if (computed.valid) dataset->info_.bounds = computed;

    quint32 routableWays = 0;
    quint32 undirectedSegments = 0;
    double roadLength = 0.0;
    QVector<quint32> edgeCounts(nodeCount + 1, 0);
    for (quint32 w = 0; w < wayCount; ++w) {
        auto& way = dataset->ways_[static_cast<int>(w)];
        way.attributes.lengthMeters = 0.0f;
        way.hasGeometry = false;
        if (way.refCount == 0) {
            way.attributes.flags |= RoadIncompleteGeometry;
            continue;
        }
        bool valid = true;
        bool boundsStarted = false;
        double length = 0.0;
        for (quint32 i = 0; i < way.refCount; ++i) {
            const quint32 nodeIndex = dataset->wayNodes_.at(static_cast<int>(way.refFirst + i));
            const auto& node = dataset->nodes_.at(static_cast<int>(nodeIndex));
            if (!containsFinitePoint(node.lat, node.lon)) {
                valid = false;
                continue;
            }
            if (!boundsStarted) {
                way.minLat = way.maxLat = node.lat;
                way.minLon = way.maxLon = node.lon;
                boundsStarted = true;
            } else {
                way.minLat = std::min(way.minLat, node.lat);
                way.minLon = std::min(way.minLon, node.lon);
                way.maxLat = std::max(way.maxLat, node.lat);
                way.maxLon = std::max(way.maxLon, node.lon);
            }
            if (i == 0) continue;
            const quint32 previousIndex = dataset->wayNodes_.at(static_cast<int>(way.refFirst + i - 1));
            const auto& previous = dataset->nodes_.at(static_cast<int>(previousIndex));
            if (previousIndex == nodeIndex) continue;
            const double segment = osmxDistanceMeters(previous.lat, previous.lon, node.lat, node.lon);
            ++undirectedSegments;
            length += segment;
            if (!way.attributes.isRoutable() || way.attributes.isArea()) continue;

            const bool oneway = way.attributes.isOneway();
            const bool reversed = way.attributes.isOnewayReversed();
            const float lengthMeters = static_cast<float>(segment);
            if (!oneway || !reversed) {
                dataset->edges_.append(OsmxEdge{previousIndex, nodeIndex, w, i - 1, lengthMeters});
            }
            if (!oneway || reversed) {
                dataset->edges_.append(OsmxEdge{nodeIndex, previousIndex, w, i - 1, lengthMeters});
            }
        }
        if (way.refCount >= 2 && valid) way.hasGeometry = true;
        if (!way.hasGeometry) way.attributes.flags |= RoadIncompleteGeometry;
        if (!way.hasGeometry) way.attributes.flags &= static_cast<quint8>(~RoadRoutable);
        way.attributes.lengthMeters = static_cast<float>(length);
        if (way.attributes.isRoutable()) {
            ++routableWays;
            roadLength += length;
        }
    }

    // Directed-edge CSR, grouped by from-node.
    for (const auto& edge : dataset->edges_) ++edgeCounts[static_cast<int>(edge.from + 1)];
    dataset->outgoingOffsets_.resize(nodeCount + 1);
    dataset->outgoingOffsets_[0] = 0;
    for (quint32 i = 0; i < nodeCount; ++i) {
        dataset->outgoingOffsets_[static_cast<int>(i + 1)] =
            dataset->outgoingOffsets_.at(static_cast<int>(i)) + edgeCounts.at(static_cast<int>(i + 1));
    }
    QVector<quint32> edgeCursor = dataset->outgoingOffsets_;
    QVector<OsmxEdge> grouped(static_cast<qsizetype>(dataset->edges_.size()));
    for (const auto& edge : dataset->edges_) {
        const quint32 slot = edgeCursor[static_cast<int>(edge.from)]++;
        grouped[static_cast<int>(slot)] = edge;
    }
    dataset->edges_ = std::move(grouped);

    dataset->info_.routableWayCount = routableWays;
    dataset->info_.undirectedSegmentCount = undirectedSegments;
    dataset->info_.directedEdgeCount = static_cast<quint32>(dataset->edges_.size());
    dataset->info_.roadLengthMeters = roadLength;

    // Uniform node grid for nearest-node queries. Cell size 0.005 degrees is
    // ~550 m in latitude and ~480 m in longitude at Wuhan's latitude.
    if (data.buildSpatialIndex && computed.valid && !dataset->nodes_.isEmpty()) {
        dataset->gridOriginLat_ = computed.minLat;
        dataset->gridOriginLon_ = computed.minLon;
        const double spanLat = computed.maxLat - computed.minLat;
        const double spanLon = computed.maxLon - computed.minLon;
        const int maxLatCell = std::max(0, static_cast<int>(std::ceil(spanLat / dataset->gridCellDegrees_)));
        const int maxLonCell = std::max(0, static_cast<int>(std::ceil(spanLon / dataset->gridCellDegrees_)));
        dataset->grid_.reserve(dataset->nodes_.size() / 4);
        for (quint32 i = 0; i < nodeCount; ++i) {
            const auto& node = dataset->nodes_.at(static_cast<int>(i));
            if (!containsFinitePoint(node.lat, node.lon)) continue;
            const int latCell = std::clamp(
                static_cast<int>((node.lat - computed.minLat) / dataset->gridCellDegrees_), 0, maxLatCell);
            const int lonCell = std::clamp(
                static_cast<int>((node.lon - computed.minLon) / dataset->gridCellDegrees_), 0, maxLonCell);
            const quint64 key = (static_cast<quint64>(static_cast<quint32>(latCell)) << 32) |
                                static_cast<quint32>(lonCell);
            dataset->grid_[key].append(i);
        }
    }

    if (error != nullptr) error->clear();
    return dataset;
}

std::optional<quint32> OsmxDataset::nodeIndex(qint64 id) const {
    const auto found = nodeIndex_.constFind(id);
    if (found == nodeIndex_.cend()) return std::nullopt;
    return found.value();
}

std::optional<quint32> OsmxDataset::wayIndex(qint64 id) const {
    const auto found = wayIndex_.constFind(id);
    if (found == wayIndex_.cend()) return std::nullopt;
    return found.value();
}

std::optional<quint32> OsmxDataset::relationIndex(qint64 id) const {
    const auto found = relationIndex_.constFind(id);
    if (found == relationIndex_.cend()) return std::nullopt;
    return found.value();
}

QString OsmxDataset::stringAt(quint32 index) const {
    if (index >= static_cast<quint32>(strings_.size())) return QString();
    return strings_.at(static_cast<int>(index));
}

QStringView OsmxDataset::stringView(quint32 index) const {
    if (index >= static_cast<quint32>(strings_.size())) return QStringView();
    return QStringView(strings_.at(static_cast<int>(index)));
}

QVector<quint32> OsmxDataset::wayNodeIndices(quint32 wayIndexValue) const {
    QVector<quint32> result;
    if (!hasWay(wayIndexValue)) return result;
    const auto& way = wayAt(wayIndexValue);
    result.resize(static_cast<qsizetype>(way.refCount));
    for (quint32 i = 0; i < way.refCount; ++i) {
        result[static_cast<int>(i)] = wayNodes_.at(static_cast<int>(way.refFirst + i));
    }
    return result;
}

QVector<qint64> OsmxDataset::wayNodeIds(quint32 wayIndexValue) const {
    QVector<qint64> result;
    if (!hasWay(wayIndexValue)) return result;
    const auto& way = wayAt(wayIndexValue);
    result.resize(static_cast<qsizetype>(way.refCount));
    for (quint32 i = 0; i < way.refCount; ++i) {
        const quint32 node = wayNodes_.at(static_cast<int>(way.refFirst + i));
        result[static_cast<int>(i)] = nodes_.at(static_cast<int>(node)).id;
    }
    return result;
}

QVector<OsmxTag> OsmxDataset::nodeTags(quint32 index) const {
    if (!hasNode(index)) return {};
    const auto& node = nodeAt(index);
    return nodeTags_.mid(static_cast<int>(node.tagFirst), static_cast<int>(node.tagCount));
}

QVector<OsmxTag> OsmxDataset::wayTags(quint32 index) const {
    if (!hasWay(index)) return {};
    const auto& way = wayAt(index);
    return wayTags_.mid(static_cast<int>(way.tagFirst), static_cast<int>(way.tagCount));
}

QVector<OsmxTag> OsmxDataset::relationTags(quint32 index) const {
    if (!hasRelation(index)) return {};
    const auto& relation = relationAt(index);
    return relationTags_.mid(static_cast<int>(relation.tagFirst), static_cast<int>(relation.tagCount));
}

QVector<OsmxMember> OsmxDataset::relationMembers(quint32 index) const {
    if (!hasRelation(index)) return {};
    const auto& relation = relationAt(index);
    return members_.mid(static_cast<int>(relation.memberFirst), static_cast<int>(relation.memberCount));
}

std::optional<QString> OsmxDataset::nodeTag(quint32 index, QStringView key) const {
    if (!hasNode(index)) return std::nullopt;
    const auto& node = nodeAt(index);
    return findTag(nodeTags_, strings_, node.tagFirst, node.tagCount, key);
}

std::optional<QString> OsmxDataset::wayTag(quint32 index, QStringView key) const {
    if (!hasWay(index)) return std::nullopt;
    const auto& way = wayAt(index);
    return findTag(wayTags_, strings_, way.tagFirst, way.tagCount, key);
}

std::optional<QString> OsmxDataset::relationTag(quint32 index, QStringView key) const {
    if (!hasRelation(index)) return std::nullopt;
    const auto& relation = relationAt(index);
    return findTag(relationTags_, strings_, relation.tagFirst, relation.tagCount, key);
}

bool OsmxDataset::nodeHasTag(quint32 index, QStringView key) const {
    return nodeTag(index, key).has_value();
}

bool OsmxDataset::wayHasTag(quint32 index, QStringView key) const {
    return wayTag(index, key).has_value();
}

QString OsmxDataset::displayName(quint32 wayIndexValue) const {
    if (!hasWay(wayIndexValue)) return QString();
    return stringAt(wayAt(wayIndexValue).attributes.name);
}

quint32 OsmxDataset::incidentWayFirst(quint32 nodeIndexValue) const {
    if (nodeIndexValue >= nodeCount()) return 0;
    return incidentOffsets_.at(static_cast<int>(nodeIndexValue));
}

quint32 OsmxDataset::incidentWayCount(quint32 nodeIndexValue) const {
    if (nodeIndexValue >= nodeCount()) return 0;
    return incidentOffsets_.at(static_cast<int>(nodeIndexValue + 1)) -
           incidentOffsets_.at(static_cast<int>(nodeIndexValue));
}

QVector<quint32> OsmxDataset::incidentWays(quint32 nodeIndexValue) const {
    QVector<quint32> result;
    const quint32 count = incidentWayCount(nodeIndexValue);
    if (count == 0) return result;
    result.resize(static_cast<qsizetype>(count));
    const quint32 first = incidentWayFirst(nodeIndexValue);
    for (quint32 i = 0; i < count; ++i) {
        result[static_cast<int>(i)] = incidentWays_.at(static_cast<int>(first + i));
    }
    return result;
}

quint32 OsmxDataset::outgoingEdgeFirst(quint32 nodeIndexValue) const {
    if (nodeIndexValue >= nodeCount()) return 0;
    return outgoingOffsets_.at(static_cast<int>(nodeIndexValue));
}

quint32 OsmxDataset::outgoingEdgeCount(quint32 nodeIndexValue) const {
    if (nodeIndexValue >= nodeCount()) return 0;
    return outgoingOffsets_.at(static_cast<int>(nodeIndexValue + 1)) -
           outgoingOffsets_.at(static_cast<int>(nodeIndexValue));
}

std::optional<quint32> OsmxDataset::nearestNode(double lat, double lon, double maxDistanceMeters,
                                                double* distanceMeters) const {
    if (distanceMeters != nullptr) *distanceMeters = -1.0;
    if (grid_.isEmpty() || !info_.bounds.valid) return std::nullopt;
    if (!std::isfinite(lat) || !std::isfinite(lon) || !(maxDistanceMeters > 0.0)) return std::nullopt;

    const int baseLatCell = static_cast<int>((lat - gridOriginLat_) / gridCellDegrees_);
    const int baseLonCell = static_cast<int>((lon - gridOriginLon_) / gridCellDegrees_);
    const double metresPerCell = gridCellDegrees_ * 111320.0;
    const int radius = std::clamp(static_cast<int>(std::ceil(maxDistanceMeters / metresPerCell)) + 1, 1,
                                  kNearestNodeMaxGridRadius);

    std::optional<quint32> best;
    double bestDistance = maxDistanceMeters;
    for (int dLat = -radius; dLat <= radius; ++dLat) {
        for (int dLon = -radius; dLon <= radius; ++dLon) {
            const int latCell = baseLatCell + dLat;
            const int lonCell = baseLonCell + dLon;
            if (latCell < 0 || lonCell < 0) continue;
            const quint64 key = (static_cast<quint64>(static_cast<quint32>(latCell)) << 32) |
                                static_cast<quint32>(lonCell);
            const auto found = grid_.constFind(key);
            if (found == grid_.cend()) continue;
            for (const quint32 index : found.value()) {
                const auto& node = nodes_.at(static_cast<int>(index));
                const double distance = osmxDistanceMeters(lat, lon, node.lat, node.lon);
                if (distance <= bestDistance) {
                    bestDistance = distance;
                    best = index;
                }
            }
        }
    }
    if (best.has_value() && distanceMeters != nullptr) *distanceMeters = bestDistance;
    return best;
}

QVector<quint32> OsmxDataset::waysInBounds(const OsmxBounds& area, int limit, bool routableOnly) const {
    QVector<quint32> result;
    if (!area.valid || limit < 0) return result;
    if (limit > 0) {
        const quint64 wanted = std::min<quint64>(static_cast<quint64>(limit), wayCount());
        result.reserve(static_cast<qsizetype>(wanted));
    }
    for (quint32 i = 0; i < wayCount(); ++i) {
        const auto& way = wayAt(i);
        if (way.refCount == 0) continue;
        if (routableOnly && !way.attributes.isRoutable()) continue;
        if (way.maxLat < area.minLat || way.minLat > area.maxLat || way.maxLon < area.minLon ||
            way.minLon > area.maxLon) {
            continue;
        }
        result.append(i);
        if (limit > 0 && result.size() >= limit) break;
    }
    return result;
}

} // namespace p1::data
