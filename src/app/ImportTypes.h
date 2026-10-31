#pragma once

#include <QMetaType>
#include <QString>

namespace p1::app {

// Statistics collected while an .osmx dataset is parsed. Values describe the
// data that was actually read and validated, never estimates.
struct ImportStats {
    quint64 nodes = 0;
    quint64 ways = 0;
    quint64 relations = 0;
    quint64 nodeReferences = 0;
    quint64 relationMembers = 0;
    quint64 tags = 0;
};

// Outcome of one import run. The struct is copied across the worker/UI thread
// boundary, therefore every field is a value type.
struct ImportResult {
    bool ok = false;
    bool cancelled = false;
    QString error;
    QString filePath;
    QString fileName;
    qint64 fileSize = 0;
    quint32 formatVersion = 0;
    QString sourceName;
    bool hasBounds = false;
    double minLat = 0.0;
    double minLon = 0.0;
    double maxLat = 0.0;
    double maxLon = 0.0;
    ImportStats stats;
    qint64 elapsedMs = 0;
};

} // namespace p1::app

Q_DECLARE_METATYPE(p1::app::ImportResult)
