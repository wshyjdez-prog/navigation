#pragma once
#include "app/ImportTypes.h"
#include <QPair>
#include <QVector>
#include <optional>
#include <atomic>
#include <memory>

// OSM source data, not a routing graph. Coordinates are WGS84 degrees; IDs
// retain signed OSM IDs (including editor-local negative IDs). Member types:
// 0 node, 1 way, 2 relation. Consumers never query the private SQLite tables.
// Convert degrees to metres in member 3's adapter, not in the importer/UI.
namespace p1::data {
class OsmxDataset;
struct OsmxBounds;
using Tags = QVector<QPair<QString, QString>>;
struct OsmNode { qint64 id; double latitude, longitude; Tags tags; };
struct OsmWay { qint64 id; QVector<qint64> nodes; Tags tags; };
struct OsmMember { int type; qint64 id; QString role; };
struct OsmRelation { qint64 id; QVector<OsmMember> members; Tags tags; };

// Each operation owns a separate read-only connection. Values own their data;
// there are no SQLite pointers or cross-thread connections in the contract.
// Invalid/corrupt caches and I/O errors throw std::runtime_error. Call off UI.
class ImportedDataset final {
public:
    explicit ImportedDataset(QString cachePath);
    app::ImportResult summary() const;
    std::optional<OsmNode> node(qint64 id) const;
    std::optional<OsmWay> way(qint64 id) const;
    std::optional<OsmRelation> relation(qint64 id) const;
    QVector<OsmNode> nodes(qint64 offset = 0, int limit = 1000) const;
    // Read-only regional road snapshot. Keeps whole highway ways with at least
    // one node in the requested WGS84 rectangle. Missing references reject the
    // way; no fabricated links across gaps. Cancellation includes SQLite scans.
    std::shared_ptr<const OsmxDataset> roads(const OsmxBounds& bounds,
        const std::shared_ptr<std::atomic_bool>& cancelled = {}) const;
private:
    QString path_;
};
QString defaultImportCacheDirectory();
QVector<app::ImportResult> listImportedDatasets(const QString& directory);
}
