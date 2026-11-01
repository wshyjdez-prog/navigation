#pragma once

#include <QString>
#include <QStringList>

#include <memory>

class QWidget;

namespace p1::ui {
class Theme;
}

namespace p1::app {

class ImportParser;

// Entry point of the OSM import module. The parser is chosen by file format
// (factory), the page is created through the same factory so the application
// shell never needs to know concrete import classes.
class ImportFactory final {
public:
    ImportFactory() = delete;

    static QStringList supportedExtensions();
    static QString fileDialogFilter();
    static bool supports(const QString& path);
    static std::unique_ptr<ImportParser> createParser(const QString& path, const QString& cacheDirectory = {});
    static QWidget* createImportPage(ui::Theme& theme, QWidget* parent = nullptr,
                                     const QString& cacheDirectory = {});
};

} // namespace p1::app
