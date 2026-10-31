#pragma once

#include "app/ImportTypes.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace p1::app {

using ImportProgressCallback = std::function<void(int percent, const QString& stage)>;
using ImportLogCallback = std::function<void(const QString& message)>;
using ImportCancelCallback = std::function<bool()>;

// One parser per supported file format (same role as an osmscout import
// module: it receives parameters, reports progress and can be cancelled).
class ImportParser {
public:
    virtual ~ImportParser() = default;

    virtual QString name() const = 0;
    virtual QStringList extensions() const = 0;

    // Parses the whole file and returns a value-type result. It must not touch
    // the UI: callbacks may be invoked from any thread. The callbacks are
    // optional; an empty callback is simply skipped.
    virtual ImportResult parse(const QString& path,
                               const ImportProgressCallback& progress,
                               const ImportLogCallback& log,
                               const ImportCancelCallback& cancelled) const = 0;
};

} // namespace p1::app
