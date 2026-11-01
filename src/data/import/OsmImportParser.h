#pragma once
#include "app/ImportParser.h"

namespace p1::data {
class OsmImportParser final : public app::ImportParser {
public:
    explicit OsmImportParser(QString cacheDirectory = {});
    static QString inputFormat(const QString& path);
    QString name() const override { return "OpenStreetMap / libosmium"; }
    QStringList extensions() const override { return {"osm","pbf","osm.pbf","osm.gz","osm.bz2"}; }
    app::ImportResult parse(const QString&, const app::ImportProgressCallback&,
        const app::ImportLogCallback&, const app::ImportCancelCallback&) const override;
private:
    QString directory_;
};
}
