#include "PoiSearchCommon.h"
#include "FeatureRules.h"
#include "PoiDedup.h"
#include <QSet>
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>

namespace p1::data {
namespace poi {
QVector<PoiHit> finish(detail::Db& db, std::vector<Candidate> found, int limit) {
    auto order = [](const Candidate& a, const Candidate& b) { return a.distance != b.distance ? a.distance < b.distance : a.id < b.id; };
    const auto keep = std::min<size_t>(size_t(limit), found.size());
    std::partial_sort(found.begin(), found.begin() + qsizetype(keep), found.end(), order);
    found.resize(keep);
    detail::Statement s(db, "SELECT osm_type,osm_id,lat,lon,category,kind,name,tags FROM pois WHERE id=?");
    QVector<PoiHit> hits;
    for (const auto& c : found) {
        s.integer(1, c.id);
        if (!s.row()) throw std::runtime_error("兴趣点缓存损坏");
        hits.append({OsmPoi{int(s.integer(0)), s.integer(1), s.real(2), s.real(3), s.text(4), s.text(5), s.text(6),
                            detail::decodeTags(s.text(7))}, c.id, c.distance});
        sqlite3_reset(s.value);
    }
    return hits;
}
}

double poiDistanceMeters(double lat1, double lon1, double lat2, double lon2) {
    constexpr double r = 6371008.8, rad = 3.14159265358979323846 / 180;
    const double dLat = (lat2 - lat1) * rad, dLon = (lon2 - lon1) * rad;
    const double a = std::sin(dLat / 2) * std::sin(dLat / 2) + std::cos(lat1 * rad) * std::cos(lat2 * rad) * std::sin(dLon / 2) * std::sin(dLon / 2);
    return 2 * r * std::asin(std::min(1.0, std::sqrt(a)));
}
QVector<PoiHit> searchNearestPois(const QString& path, const QString& rawText, double lat, double lon,
    int limit, PoiSearchStats* out, const std::shared_ptr<std::atomic_bool>& cancel) {
    const auto text = rawText.trimmed();
    if (text.isEmpty() || limit < 1 || limit > 1000 || !std::isfinite(lat) || !std::isfinite(lon) || std::abs(lat) > 90 || std::abs(lon) > 180)
        throw std::invalid_argument("Invalid POI search");
    poi::check(cancel);
    PoiSearchStats stats;
    const auto hits = poi::searchMemory(path, text, lat, lon, limit, stats, cancel);
    if (out) *out = stats;
    return hits;
}
QVector<PoiHit> searchNearestPoisByScan(const QString& path, const QString& rawText, double lat, double lon, int limit) {
    const auto text = rawText.trimmed();
    detail::Db db(path, true);
    std::vector<detail::PoiRow> rows;
    {
        detail::Statement s(db, "SELECT id,name,lat,lon FROM pois WHERE name<>''");
        while (s.row()) rows.push_back({s.integer(0), s.text(1), s.real(2), s.real(3)});
    }
    QSet<qint64> merged;
    if (!detail::poisDeduplicated(db)) for (auto id : detail::duplicatePoiIds(rows)) merged.insert(id);
    std::vector<poi::Candidate> found;
    for (const auto& r : rows)
        if (!merged.contains(r.id) && poi::nameMatches(r.name, text)) found.push_back({r.id, poiDistanceMeters(lat, lon, r.lat, r.lon)});
    return poi::finish(db, std::move(found), limit);
}
}
