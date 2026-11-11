#pragma once
// Shared, private rules of the import module: which OSM tags make an area or a
// POI, and how feature geometry is serialised into the SQLite cache. The
// libosmium importer writes with these helpers; the cache readers use the same
// ones, so the on-disk layout has exactly one definition.
#include "contracts/MapGeometry.h"
#include <QByteArray>
#include <QDataStream>
#include <QHash>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <optional>
#include <stdexcept>

namespace p1::data::detail {
inline constexpr char kCacheSchema[] = "2";
using TagMap = QHash<QString, QString>;

inline bool tagSet(const TagMap& t, const char* key) {
    const auto it = t.constFind(QLatin1String(key));
    return it != t.cend() && !it->isEmpty() && *it != QLatin1String("no");
}
inline QString displayName(const TagMap& t) {
    const auto local = t.value(QStringLiteral("name:zh"));
    return local.isEmpty() ? t.value(QStringLiteral("name")) : local;
}

// Polygon themes drawn by the map. Anything else is not stored as an area.
inline std::optional<MapArea> classifyArea(const TagMap& t) {
    if (t.value("area") == "no") return {};
    MapArea a; a.name = displayName(t);
    const auto land = t.value("landuse"), natural = t.value("natural"), leisure = t.value("leisure");
    if (tagSet(t, "building") || tagSet(t, "building:part")) { a.kind = MapAreaKind::Building; a.minZoom = 15; }
    else if (natural == "water" || t.value("waterway") == "riverbank" || land == "reservoir" || land == "basin") {
        a.kind = MapAreaKind::Water; a.minZoom = 8;
    } else if (natural == "wood" || natural == "scrub" || natural == "grassland" || natural == "heath" ||
               natural == "wetland" || land == "forest" || land == "grass" || land == "meadow" ||
               land == "recreation_ground" || land == "village_green" || leisure == "park" ||
               leisure == "garden" || leisure == "golf_course") {
        a.kind = MapAreaKind::Green; a.minZoom = 10;
    } else if (tagSet(t, "landuse")) { a.kind = MapAreaKind::Landuse; a.minZoom = 11; }
    else return {};
    return a;
}

// POI rule: the first matching key is the category, its value the kind.
// railway/aeroway only count for passenger-facing objects, not tracks/runways.
struct PoiClass { QString category, kind; };
inline constexpr const char* kPoiKeys[] = {"amenity", "shop", "tourism", "leisure", "historic", "office",
                                           "healthcare", "craft", "emergency", "public_transport"};
inline std::optional<PoiClass> classifyPoi(const TagMap& t) {
    for (const char* key : kPoiKeys)
        if (tagSet(t, key)) return PoiClass{QString::fromLatin1(key), t.value(QLatin1String(key))};
    const auto railway = t.value("railway");
    if (railway == "station" || railway == "halt" || railway == "tram_stop" || railway == "subway_entrance")
        return PoiClass{"railway", railway};
    const auto aeroway = t.value("aeroway");
    if (aeroway == "aerodrome" || aeroway == "terminal") return PoiClass{"aeroway", aeroway};
    return {};
}

inline QString encodeTags(const QVector<QPair<QString, QString>>& tags) {
    QJsonArray values;
    for (const auto& tag : tags) values.append(QJsonArray{tag.first, tag.second});
    return QString::fromUtf8(QJsonDocument(values).toJson(QJsonDocument::Compact));
}
inline QVector<QPair<QString, QString>> decodeTags(const QString& text) {
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray()) throw std::runtime_error("导入缓存标签损坏");
    QVector<QPair<QString, QString>> result;
    for (const auto& item : doc.array()) {
        const auto pair = item.toArray();
        if (pair.size() != 2 || !pair[0].isString() || !pair[1].isString())
            throw std::runtime_error("导入缓存标签损坏");
        result.append(qMakePair(pair[0].toString(), pair[1].toString()));
    }
    return result;
}

// Geometry blobs: QDataStream (Qt_6_4, big-endian, double precision), points
// are QPointF(lon, lat) in WGS84 degrees.
template <typename... T> QByteArray pack(const T&... values) {
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_4); (stream << ... << values);
    return bytes;
}
template <typename... T> void unpack(const QByteArray& bytes, T&... values) {
    QDataStream stream(bytes); stream.setVersion(QDataStream::Qt_6_4); (stream >> ... >> values);
    if (stream.status() != QDataStream::Ok || !stream.atEnd()) throw std::runtime_error("导入缓存几何损坏");
}
}
