// POI name index (DEC-20261111-1-poi-index-01): in-memory character inverted index.
#include "PoiSearchCommon.h"
#include "PoiDedup.h"
#include <QSet>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <algorithm>

namespace p1::data::poi {
namespace {
struct Entry { qint64 id; double lat, lon; QString name; };
struct MemoryIndex {
    std::vector<Entry> entries;
    QHash<char32_t, std::vector<quint32>> postings; // lower-cased code point -> entry indices (ascending)
};
// One immutable index per cache file version, shared by all searches.
std::shared_ptr<const MemoryIndex> index(const QString& path, PoiSearchStats& stats, const std::shared_ptr<std::atomic_bool>& cancel) {
    static QMutex mutex;
    static QHash<QString, std::shared_ptr<const MemoryIndex>> cache;
    const QFileInfo info(path);
    const auto key = info.absoluteFilePath() + "@" + QString::number(info.lastModified().toMSecsSinceEpoch()) + "/" + QString::number(info.size());
    QMutexLocker lock(&mutex);
    if (auto it = cache.find(key); it != cache.end()) return it.value();
    QElapsedTimer timer; timer.start();
    auto built = std::make_shared<MemoryIndex>();
    detail::Db db(path, true);
    // Caches imported before the post-import check get the same merge here, in memory.
    QSet<qint64> merged;
    if (!detail::poisDeduplicated(db)) {
        std::vector<detail::PoiRow> rows;
        detail::Statement all(db, "SELECT id,name,lat,lon FROM pois WHERE name<>''");
        while (all.row()) rows.push_back({all.integer(0), all.text(1), all.real(2), all.real(3)});
        for (auto id : detail::duplicatePoiIds(std::move(rows), detail::kPoiDuplicateMeters, [&] { check(cancel); })) merged.insert(id);
    }
    detail::Statement s(db, "SELECT id,name,lat,lon FROM pois WHERE name<>'' ORDER BY id");
    while (s.row()) {
        if (built->entries.size() % 4096 == 0) check(cancel);
        if (merged.contains(s.integer(0))) continue;
        const auto i = quint32(built->entries.size());
        built->entries.push_back({s.integer(0), s.real(2), s.real(3), s.text(1)});
        auto points = built->entries.back().name.toLower().toUcs4();
        std::sort(points.begin(), points.end());
        points.erase(std::unique(points.begin(), points.end()), points.end());
        for (auto p : points) built->postings[p].push_back(i);
    }
    stats.builtIndex = true;
    stats.buildMs = timer.nsecsElapsed() / 1e6;
    cache.insert(key, built);
    return built;
}
}
QVector<PoiHit> searchMemory(const QString& path, const QString& text, double lat, double lon, int limit,
    PoiSearchStats& stats, const std::shared_ptr<std::atomic_bool>& cancel) {
    const auto idx = index(path, stats, cancel);
    QElapsedTimer timer; timer.start();
    // The rarest character's posting list bounds the candidates.
    const std::vector<quint32>* rarest = nullptr;
    for (auto p : text.toLower().toUcs4()) {
        const auto it = idx->postings.constFind(p);
        if (it == idx->postings.cend()) { rarest = nullptr; stats.queryMs = timer.nsecsElapsed() / 1e6; return {}; }
        if (!rarest || it->size() < rarest->size()) rarest = &*it;
    }
    std::vector<Candidate> found;
    if (rarest) for (auto i : *rarest) {
        if (++stats.candidates % 4096 == 0) check(cancel);
        const auto& e = idx->entries[i];
        if (nameMatches(e.name, text)) found.push_back({e.id, poiDistanceMeters(lat, lon, e.lat, e.lon)});
    }
    detail::Db db(path, true);
    auto hits = finish(db, std::move(found), limit);
    stats.queryMs = timer.nsecsElapsed() / 1e6;
    return hits;
}
}
