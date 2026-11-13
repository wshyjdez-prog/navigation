#pragma once
#include "Sqlite.h"
#include <QString>
#include <functional>
#include <vector>

namespace p1::data::detail {
// Post-import POI check (DEC-20261111-1-poi-dedup-01, rule set by member 1):
// named POIs with an identical (trimmed) name lying within kPoiDuplicateMeters
// of an already kept POI are the same place and are merged into it. POIs are
// visited in id order, so each cluster keeps its smallest id; the result is
// deterministic. Unnamed POIs are never merged.
constexpr double kPoiDuplicateMeters = 100;
struct PoiRow { qint64 id; QString name; double lat, lon; };
// Ids to drop (ascending). `check` may throw to cancel.
std::vector<qint64> duplicatePoiIds(std::vector<PoiRow> rows, double metres = kPoiDuplicateMeters,
    const std::function<void()>& check = {});
// Deletes the duplicates from pois and records metadata poi_dedup_m.
// Runs inside the caller's import transaction; returns the number removed.
quint64 removeDuplicatePois(Db& db, const std::function<void()>& check = {});
// True when the cache was deduplicated at import (caches imported earlier are not).
bool poisDeduplicated(Db& db);
}
