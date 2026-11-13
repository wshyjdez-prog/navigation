#pragma once
#include "contracts/ImportedDataset.h"
#include <atomic>
#include <memory>

namespace p1::data {
// Nearest-name POI search for the map (DEC-20261111-1-poi-index-01).
// Result: POIs whose name contains `text` (QString::contains,
// case-insensitive), ranked by great-circle distance to the origin, ties by
// row id; at most `limit` hits. Index: built on first use when missing, an
// in-memory character inverted index (lower-cased code point -> POIs whose
// name contains it), cached per cache-file version for the life of the
// process. The posting list of the query's rarest character bounds the
// candidates; each candidate is then checked with contains().
// Runs on the caller's worker; no shared connection.
struct PoiHit { OsmPoi poi; qint64 id=0; double distanceM=0; };
struct PoiSearchStats {
    bool builtIndex=false;   // this call created the index
    double buildMs=0;        // index creation time when builtIndex
    double queryMs=0;        // search time excluding index creation
    quint64 candidates=0;    // rows the index handed to the exact name check
};
QVector<PoiHit> searchNearestPois(const QString& cachePath, const QString& text,
    double originLat, double originLon, int limit = 20, PoiSearchStats* stats = nullptr,
    const std::shared_ptr<std::atomic_bool>& cancel = {});
// Reference implementation (full scan, no index); used by tests.
QVector<PoiHit> searchNearestPoisByScan(const QString& cachePath, const QString& text,
    double originLat, double originLon, int limit = 20);
double poiDistanceMeters(double lat1, double lon1, double lat2, double lon2);
}
