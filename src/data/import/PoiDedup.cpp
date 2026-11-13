#include "PoiDedup.h"
#include <QHash>
#include <algorithm>
#include <cmath>

namespace p1::data::detail {
namespace {
double metresBetween(double lat1, double lon1, double lat2, double lon2) {
    constexpr double r = 6371008.8, rad = 3.14159265358979323846 / 180;
    const double dLat = (lat2 - lat1) * rad, dLon = (lon2 - lon1) * rad;
    const double a = std::sin(dLat / 2) * std::sin(dLat / 2) + std::cos(lat1 * rad) * std::cos(lat2 * rad) * std::sin(dLon / 2) * std::sin(dLon / 2);
    return 2 * r * std::asin(std::min(1.0, std::sqrt(a)));
}
}
std::vector<qint64> duplicatePoiIds(std::vector<PoiRow> rows, double metres, const std::function<void()>& check) {
    std::sort(rows.begin(), rows.end(), [](const PoiRow& a, const PoiRow& b) { return a.id < b.id; });
    // Kept POIs per name, bucketed on a ~metres grid so a check only looks at
    // the 3x3 neighbouring cells instead of every POI with that name.
    const double cellLat = metres / 111195.08;
    QHash<QString, QHash<quint64, std::vector<const PoiRow*>>> kept;
    std::vector<qint64> drop;
    size_t n = 0;
    for (const auto& row : rows) {
        if (check && ++n % 4096 == 0) check();
        const auto name = row.name.trimmed();
        if (name.isEmpty()) continue;
        const double cellLon = cellLat / std::max(0.01, std::cos(row.lat * 3.14159265358979323846 / 180));
        const auto cy = qint64(std::floor(row.lat / cellLat)), cx = qint64(std::floor(row.lon / cellLon));
        auto& cells = kept[name];
        bool duplicate = false;
        for (qint64 y = cy - 1; y <= cy + 1 && !duplicate; ++y)
            for (qint64 x = cx - 1; x <= cx + 1 && !duplicate; ++x)
                for (const auto* other : cells.value((quint64(quint32(y)) << 32) | quint32(x)))
                    if (metresBetween(row.lat, row.lon, other->lat, other->lon) <= metres) { duplicate = true; break; }
        if (duplicate) drop.push_back(row.id);
        else cells[(quint64(quint32(cy)) << 32) | quint32(cx)].push_back(&row);
    }
    return drop;
}
quint64 removeDuplicatePois(Db& db, const std::function<void()>& check) {
    std::vector<PoiRow> rows;
    {
        Statement s(db, "SELECT id,name,lat,lon FROM pois WHERE name<>''");
        while (s.row()) rows.push_back({s.integer(0), s.text(1), s.real(2), s.real(3)});
    }
    const auto drop = duplicatePoiIds(std::move(rows), kPoiDuplicateMeters, check);
    Statement del(db, "DELETE FROM pois WHERE id=?");
    for (auto id : drop) { del.integer(1, id); del.insert(); }
    Statement meta(db, "INSERT OR REPLACE INTO metadata VALUES('poi_dedup_m',?)");
    meta.text(1, QString::number(kPoiDuplicateMeters)); meta.insert();
    return drop.size();
}
bool poisDeduplicated(Db& db) {
    Statement s(db, "SELECT 1 FROM metadata WHERE key='poi_dedup_m'");
    return s.row();
}
}
