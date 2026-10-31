#include "app/ImportFactory.h"

#include "app/ImportPage.h"
#include "app/OsmxParser.h"

#include <QFileInfo>

#include <utility>

namespace p1::app {

QStringList ImportFactory::supportedExtensions() {
    // A new format only needs its parser here plus an entry in this list.
    return {QStringLiteral("osmx")};
}

QString ImportFactory::fileDialogFilter() {
    return QStringLiteral("OSMX 数据文件 (*.osmx);;所有文件 (*)");
}

bool ImportFactory::supports(const QString& path) {
    const QString suffix = QFileInfo(path).suffix();
    return !suffix.isEmpty() && supportedExtensions().contains(suffix, Qt::CaseInsensitive);
}

std::unique_ptr<ImportParser> ImportFactory::createParser(const QString& path) {
    if (!supports(path)) return nullptr;
    return std::make_unique<OsmxParser>();
}

QWidget* ImportFactory::createImportPage(ui::Theme& theme, QWidget* parent) {
    return new ImportPage(theme, parent);
}

} // namespace p1::app
