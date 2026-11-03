#pragma once

#include "app/ImportParser.h"
#include "data/osmx/OsmxStore.h"

namespace p1::app {

// ImportParser adapter for the unified OSMX road-network format. It parses on
// the worker thread, writes the local cache (data.osmx + manifest.json) and
// publishes the immutable dataset to OsmxStore so 3 (A*/HMM) and 5 (renderer)
// read the same instance through p1::data::OsmxStore::shared().active().
class OsmxImportAdapter final : public ImportParser {
public:
    explicit OsmxImportAdapter(QString cacheDirectory = {}, data::OsmxStore* store = nullptr);

    static QString inputFormat(const QString& path);

    QString name() const override { return QStringLiteral("OSMX 统一路网"); }
    QStringList extensions() const override { return {QStringLiteral("osmx")}; }

    ImportResult parse(const QString& path, const ImportProgressCallback& progress,
                       const ImportLogCallback& log, const ImportCancelCallback& cancelled) const override;

private:
    QString directory_;
    data::OsmxStore* store_ = nullptr;
};

} // namespace p1::app
