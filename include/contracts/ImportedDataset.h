#pragma once
#include "app/ImportTypes.h"
#include <QPair>
#include <QVector>
#include <optional>
#include <atomic>
#include <memory>

// Features extracted from standard OSM data by the libosmium importer: POIs,
// roads and areas. The cache is not a mirror of raw OSM nodes/ways/relations.
// Coordinates are WGS84 degrees; IDs retain signed OSM IDs. Consumers never
// query the private SQLite tables. Convert degrees to metres in member 3's
// adapter, not in the importer/UI. Areas are read through MapGeometry.h.
namespace p1::data {
class OsmxDataset;
struct OsmxBounds;
using Tags = QVector<QPair<QString, QString>>;
// A point of interest. osmType: 0 node, 1 way, 2 relation; for ways and
// multipolygons the position is the centre of the polygon's bounding box.
// category is the defining OSM key (amenity, shop, tourism, ...), kind its
// value (restaurant, supermarket, ...). name prefers name:zh and may be empty.
struct OsmPoi { int osmType; qint64 osmId; double latitude, longitude; QString category, kind, name; Tags tags; };

// Each operation owns a separate read-only connection. Values own their data;
// there are no SQLite pointers or cross-thread connections in the contract.
// Invalid/corrupt caches and I/O errors throw std::runtime_error. Call off UI.
class ImportedDataset final {
public:
    explicit ImportedDataset(QString cachePath);
    app::ImportResult summary() const;
    // POIs inside a WGS84 rectangle, optionally of one category, ordered by
    // cache row. limit is 1..10000. POIs are data for search and lists only:
    // the map renderer does not draw them.
    QVector<OsmPoi> pois(const OsmxBounds& bounds, const QString& category = {}, int limit = 1000) const;
    // Case-insensitive substring match on the display name. limit is 1..1000.
    QVector<OsmPoi> findPois(const QString& nameText, int limit = 100) const;
    // Read-only regional road snapshot. Keeps whole highway ways with at least
    // one node in the requested WGS84 rectangle. Ways with missing node
    // references were rejected at import; no fabricated links across gaps.
    // Browsing may opt into an empty snapshot; routing keeps empty-area errors.
    std::shared_ptr<const OsmxDataset> roads(const OsmxBounds& bounds,
        const std::shared_ptr<std::atomic_bool>& cancelled = {}, bool allowEmpty = false) const;
private:
    QString path_;
};
QString defaultImportCacheDirectory();
// Caches written by the previous raw-OSM importer (schema 1) are not listed;
// the source file has to be imported again.
QVector<app::ImportResult> listImportedDatasets(const QString& directory);
}
