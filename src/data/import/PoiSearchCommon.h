#pragma once
// Internal helpers of the POI search.
#include "PoiSearch.h"
#include "Sqlite.h"
#include <vector>

namespace p1::data::poi {
struct Candidate { qint64 id; double distance; };
inline void check(const std::shared_ptr<std::atomic_bool>& c) { if (c && c->load()) throw std::runtime_error("已取消兴趣点搜索"); }
inline bool nameMatches(const QString& name, const QString& text) { return name.contains(text, Qt::CaseInsensitive); }
// Keeps the `limit` nearest (distance, id) and loads their full rows.
QVector<PoiHit> finish(detail::Db& db, std::vector<Candidate> found, int limit);
QVector<PoiHit> searchMemory(const QString& path, const QString& text, double lat, double lon, int limit, PoiSearchStats& stats, const std::shared_ptr<std::atomic_bool>& cancel);
}
