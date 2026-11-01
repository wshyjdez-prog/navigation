#include "app/ImportFactory.h"

#include "app/ImportPage.h"
#include "data/import/OsmImportParser.h"

#include <QFileInfo>

#include <utility>

namespace p1::app {

QStringList ImportFactory::supportedExtensions() {
    return {"osm","pbf","osm.pbf","osm.gz","osm.bz2"};
}

QString ImportFactory::fileDialogFilter() {
    return QStringLiteral("OpenStreetMap 数据 (*.osm *.pbf *.osm.pbf *.osm.gz *.osm.bz2);;所有文件 (*)");
}

bool ImportFactory::supports(const QString& path) {
    return !data::OsmImportParser::inputFormat(path).isEmpty();
}

std::unique_ptr<ImportParser> ImportFactory::createParser(const QString& path, const QString& cacheDirectory) {
    if (!supports(path)) return nullptr;
    return std::make_unique<data::OsmImportParser>(cacheDirectory);
}

QWidget* ImportFactory::createImportPage(ui::Theme& theme, QWidget* parent, const QString& cacheDirectory) {
    return new ImportPage(theme, parent, cacheDirectory);
}

} // namespace p1::app
