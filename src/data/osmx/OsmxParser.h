#pragma once

#include "OsmxDataset.h"

#include <QStringList>

#include <functional>
#include <memory>

namespace p1::data {

struct OsmxParseOptions {
    // Builds the uniform node grid used by nearestNode(). Disable only for
    // memory-constrained validation tools.
    bool buildSpatialIndex = true;
    // Empty means a fresh UUID is generated for this load.
    QString datasetId;
    // ISO-8601 UTC import timestamp. Empty means "now". Re-opening a cached
    // dataset passes the manifest value so the shared instance keeps the
    // original import time instead of the load time.
    QString importedAt;
    // A v1 file ends exactly after the relation table. Left as an option so a
    // future compatible trailer can be negotiated without rewriting callers.
    bool rejectTrailingBytes = true;
};

struct OsmxParseCallbacks {
    // Called from the calling thread at a monotonic, coarse percentage. The
    // stage text is user-facing Chinese and may change between versions.
    std::function<void(int percent, const QString& stage)> progress;
    std::function<void(const QString& message)> log;
    // Polled frequently; returning true aborts the load without publishing a
    // dataset. The callable must be thread-safe if the GUI uses it cross-thread.
    std::function<bool()> cancelled;
};

struct OsmxParseResult {
    std::shared_ptr<const OsmxDataset> dataset;
    OsmxError error;
    QString sourcePath;
    QString sha256;
    qint64 fileBytes = 0;
    qint64 elapsedMs = 0;
    QStringList warnings;
    bool cancelled = false;

    bool ok() const { return dataset != nullptr; }
};

// Reads one OSMX v1 file into an immutable OsmxDataset. The parser validates
// the header, counts, UTF-16 strings, coordinates, duplicate IDs and every
// structural slice before publishing; malformed input yields an OsmxError with
// a byte offset instead of a partially usable dataset. All work happens on the
// calling thread; call it from a worker (ImportTask does) when the file is
// large. No exceptions escape parse().
class OsmxParser final {
public:
    OsmxParser() = default;

    // Returns "osmx" for a .osmx path, otherwise an empty string. Matching is
    // case-insensitive and does not inspect the file content.
    static QString inputFormat(const QString& path);
    static bool supports(const QString& path);

    // Cheap header-only contract check for file pickers and diagnostics. It
    // does not validate the object tables.
    static OsmxError validateHeader(const QString& path);

    OsmxParseResult parse(const QString& path, const OsmxParseOptions& options = {},
                          const OsmxParseCallbacks& callbacks = {}) const;
};

} // namespace p1::data
