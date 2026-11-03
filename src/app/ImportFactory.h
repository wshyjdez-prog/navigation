#pragma once

#include "app/ImportTypes.h"

#include <QString>
#include <QStringList>

#include <memory>

class QWidget;

namespace p1::ui {
class Theme;
}

namespace p1::app {

class ImportParser;

// Entry point of the OSM/OSMX import module. The parser is chosen by file
// format (factory), the page is created through the same factory so the
// application shell never needs to know concrete import classes. The page has
// the stable route ID "import" and is registered with
// MainWindow::registerPage(pageId(), pageTitle(), pageDescription(), page).
class ImportFactory final {
public:
    ImportFactory() = delete;

    static QString pageId();
    static QString pageTitle();
    static QString pageDescription();
    static QStringList supportedExtensions();
    static QString fileDialogFilter();
    static bool supports(const QString& path);
    static std::unique_ptr<ImportParser> createParser(const QString& path, const QString& cacheDirectory = {});
    // "Open again" path for a dataset that is already in the local cache: the
    // catalog row (OsmxCache::list entry) identifies it, and the loader
    // re-parses <cachePath>/data.osmx and publishes it to OsmxStore. Returns
    // nullptr for non-OSMX rows or an unsafe/missing dataset id.
    static bool canReopenCachedDataset(const ImportResult& entry);
    static std::unique_ptr<ImportParser> createDatasetLoader(const ImportResult& entry);
    // GUI thread only; returns nullptr when called from another thread. On
    // success the caller owns the page until registerPage() accepts it.
    static QWidget* createImportPage(ui::Theme& theme, QWidget* parent = nullptr,
                                     const QString& cacheDirectory = {});
};

} // namespace p1::app
