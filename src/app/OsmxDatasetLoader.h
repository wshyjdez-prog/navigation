#pragma once

#include "app/ImportParser.h"
#include "data/osmx/OsmxStore.h"

namespace p1::app {

// ImportParser for a dataset that already lives in the local OSMX cache. It is
// the "open again" path: after a restart the catalog lists what was imported
// earlier, but the process-wide OsmxStore is empty until one of those datasets
// is parsed again. The loader re-reads <cachePath>/data.osmx, verifies it
// against the manifest SHA-256 and publishes it to OsmxStore, so 3 (A*/HMM) and
// 5 (renderer) again share one instance.
//
// It never writes the cache; a missing or modified file is reported as an
// error instead of silently replacing local data.
class OsmxDatasetLoader final : public ImportParser {
public:
    // entry is a catalog row produced by OsmxCache::list() (datasetId,
    // cachePath, importedAt, sourceSha256, counts). store defaults to the
    // process-wide OsmxStore.
    explicit OsmxDatasetLoader(ImportResult entry, data::OsmxStore* store = nullptr);

    QString name() const override { return QStringLiteral("OSMX 本地数据集"); }
    QStringList extensions() const override { return {QStringLiteral("osmx")}; }

    // The path argument is ignored: the loader always reads
    // OsmxCache::dataPath(entry.cachePath, entry.datasetId), so a mismatched
    // caller path can never read outside the dataset directory. It is parsed
    // with the catalog datasetId and import time so the shared dataset keeps its
    // identity across restarts.
    ImportResult parse(const QString& path, const ImportProgressCallback& progress,
                       const ImportLogCallback& log, const ImportCancelCallback& cancelled) const override;

private:
    ImportResult entry_;
    data::OsmxStore* store_ = nullptr;
};

} // namespace p1::app
