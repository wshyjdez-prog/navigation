#include "app/ImportFactory.h"

#include "app/ImportPage.h"
#include "app/OsmxDatasetLoader.h"
#include "app/OsmxImportAdapter.h"
#include "data/import/OsmImportParser.h"
#include "data/osmx/OsmxCache.h"
#include "data/osmx/OsmxParser.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QThread>

#include <utility>

namespace p1::app {

QString ImportFactory::pageId() {
    return QStringLiteral("import");
}

QString ImportFactory::pageTitle() {
    return QStringLiteral("数据导入");
}

QString ImportFactory::pageDescription() {
    return QStringLiteral("导入 OSMX 统一路网或标准 OSM 数据，并保存到本机。");
}

QStringList ImportFactory::supportedExtensions() {
    return {"osmx", "osm", "pbf", "osm.pbf", "osm.gz", "osm.bz2"};
}

QString ImportFactory::fileDialogFilter() {
    return QStringLiteral(
        "地图数据 (*.osmx *.osm *.pbf *.osm.pbf *.osm.gz *.osm.bz2);;"
        "OSMX 统一路网 (*.osmx);;OpenStreetMap 数据 (*.osm *.pbf *.osm.pbf *.osm.gz *.osm.bz2);;"
        "所有文件 (*)");
}

bool ImportFactory::supports(const QString& path) {
    return data::OsmxParser::supports(path) || !data::OsmImportParser::inputFormat(path).isEmpty();
}

std::unique_ptr<ImportParser> ImportFactory::createParser(const QString& path, const QString& cacheDirectory) {
    if (data::OsmxParser::supports(path)) return std::make_unique<OsmxImportAdapter>(cacheDirectory);
    if (!data::OsmImportParser::inputFormat(path).isEmpty()) {
        return std::make_unique<data::OsmImportParser>(cacheDirectory);
    }
    return nullptr;
}

bool ImportFactory::canReopenCachedDataset(const ImportResult& entry) {
    if (entry.sourceFormat.compare(QLatin1String("osmx"), Qt::CaseInsensitive) != 0) return false;
    if (!data::OsmxCache::isSafeDatasetId(entry.datasetId)) return false;
    return !entry.cachePath.isEmpty();
}

std::unique_ptr<ImportParser> ImportFactory::createDatasetLoader(const ImportResult& entry) {
    if (!canReopenCachedDataset(entry)) return nullptr;
    return std::make_unique<OsmxDatasetLoader>(entry);
}

QWidget* ImportFactory::createImportPage(ui::Theme& theme, QWidget* parent, const QString& cacheDirectory) {
    // The page owns worker threads and Qt widgets, so it must be created by the
    // GUI thread. Returning nullptr keeps ownership rules explicit: the caller
    // must not assume a page was created.
    if (QCoreApplication::instance() != nullptr &&
        QThread::currentThread() != QCoreApplication::instance()->thread()) {
        qWarning("ImportFactory::createImportPage must be called from the GUI thread");
        return nullptr;
    }
    return new ImportPage(theme, parent, cacheDirectory);
}

} // namespace p1::app
