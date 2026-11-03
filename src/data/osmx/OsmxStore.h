#pragma once

#include "OsmxDataset.h"

#include <QHash>
#include <QMutex>
#include <QString>
#include <QVector>

#include <memory>

namespace p1::data {

// Process-wide, read-only hand-off point for the currently loaded OSMX data.
// The import page publishes one immutable dataset after a successful load; 3
// (A*/HMM) and 5 (renderer) read the same shared_ptr, so both consume exactly
// the same nodes/ways/edges and unit conventions instead of re-parsing the file.
//
// Thread-safety: all methods are safe from any thread. Datasets are immutable,
// so a snapshot returned by active()/find() stays valid even if a newer dataset
// is published. This class never mutates a published dataset.
class OsmxStore final {
public:
    static OsmxStore& shared();

    // Publishes the dataset and makes it active. A null pointer is ignored.
    void publish(std::shared_ptr<const OsmxDataset> dataset);

    std::shared_ptr<const OsmxDataset> active() const;
    std::shared_ptr<const OsmxDataset> find(const QString& datasetId) const;
    QString activeId() const;
    QVector<QString> datasetIds() const;

    // Test/session cleanup. Published snapshots held by consumers remain valid.
    void clear();

    // Number of datasets retained for find(); the oldest inactive entries are
    // evicted first. Active dataset is never evicted.
    static constexpr int kRetainLimit = 4;

private:
    OsmxStore() = default;

    mutable QMutex mutex_;
    QHash<QString, std::shared_ptr<const OsmxDataset>> datasets_;
    QVector<QString> order_;
    QString activeId_;
};

} // namespace p1::data
