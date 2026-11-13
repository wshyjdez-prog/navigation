#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>

namespace p1::app {

// Statistics collected while an OSM dataset is parsed. Values describe the
// data that was actually read and validated, never estimates.
struct ImportStats {
    quint64 nodes = 0;
    quint64 ways = 0;
    quint64 relations = 0;
    quint64 nodeReferences = 0;
    quint64 relationMembers = 0;
    quint64 tags = 0;
    // Standard OSM (libosmium) only: features extracted into the cache. POIs
    // are stored for search/lists and are deliberately not drawn on the map.
    quint64 pois = 0;
    quint64 roads = 0;
    quint64 areas = 0;
    quint64 areasSkipped = 0;
    // POIs merged by the post-import check: same name within 100 m.
    quint64 poisMerged = 0;
    // OSMX only: precomputed routing topology shared with 3/5. Zero for other
    // formats, where the consumer derives its own graph.
    quint64 routableWays = 0;
    quint64 directedEdges = 0;
};

// Outcome of one import run. The struct is copied across the worker/UI thread
// boundary, therefore every field is a value type.
struct ImportResult {
    bool ok = false;
    bool cancelled = false;
    QString error;
    QString filePath;
    QString fileName;
    qint64 fileSize = 0;
    quint32 formatVersion = 0;
    QString sourceFormat;
    QString datasetId;
    QString cachePath;
    QString sourceSha256;
    QString importedAt;
    QStringList warnings;
    QString sourceName;
    bool hasBounds = false;
    double minLat = 0.0;
    double minLon = 0.0;
    double maxLat = 0.0;
    double maxLon = 0.0;
    ImportStats stats;
    qint64 elapsedMs = 0;
};

} // namespace p1::app

Q_DECLARE_METATYPE(p1::app::ImportResult)
