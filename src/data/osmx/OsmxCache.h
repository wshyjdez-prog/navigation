#pragma once

#include "OsmxParser.h"
#include "app/ImportTypes.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace p1::data {

// Result of writing one loaded dataset into the local cache.
struct OsmxInstallResult {
    bool ok = false;
    bool cancelled = false;
    QString datasetDirectory;
    QString error;
    qint64 elapsedMs = 0;
};

// Listing of cached OSMX datasets. Corrupt entries are skipped and described in
// problems instead of aborting the whole directory scan.
struct OsmxCacheListing {
    QVector<app::ImportResult> entries;
    QStringList problems;
};

// Local OSMX storage convention (see docs/osmx-storage.md):
//   <root>/<datasetId>/data.osmx
//   <root>/<datasetId>/manifest.json
// A dataset directory is published by staging the copy and renaming the staging
// directory, so a crash/cancel never leaves a half-import that the catalog
// accepts. Consumers read the manifest (cheap) or open data.osmx via
// OsmxParser; they never assume a database.
class OsmxCache final {
public:
    OsmxCache() = delete;

    static QString defaultDirectory();

    // Copies sourceFile into directory/<datasetId>/ and writes the manifest.
    // The copy is verified against dataset.info().sourceSha256. Progress is
    // reported from the calling thread at 0..100 for the cache phase only.
    static OsmxInstallResult install(const OsmxDataset& dataset, const QString& sourceFile,
                                     const QString& directory, const OsmxParseCallbacks& callbacks = {});

    static OsmxCacheListing list(const QString& directory);

    static QString datasetDirectory(const QString& directory, const QString& datasetId);
    static QString dataPath(const QString& directory, const QString& datasetId);
    static QString manifestPath(const QString& directory, const QString& datasetId);

    // Deletes one published dataset directory. Returns false with a reason when
    // datasetId is unsafe or removal fails.
    static bool remove(const QString& directory, const QString& datasetId, QString* error = nullptr);

    // Dataset IDs are generated UUIDs (without braces) and are also used as
    // directory names, so only [A-Za-z0-9_-] is accepted.
    static bool isSafeDatasetId(const QString& id);
};

} // namespace p1::data
