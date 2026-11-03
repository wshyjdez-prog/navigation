#include "OsmxStore.h"

#include <QMutexLocker>

#include <utility>

namespace p1::data {

OsmxStore& OsmxStore::shared() {
    static OsmxStore store;
    return store;
}

void OsmxStore::publish(std::shared_ptr<const OsmxDataset> dataset) {
    if (!dataset) return;
    const QString id = dataset->info().datasetId;
    if (id.isEmpty()) return;
    QMutexLocker locker(&mutex_);
    if (!datasets_.contains(id)) order_.append(id);
    datasets_.insert(id, std::move(dataset));
    activeId_ = id;
    // Evict the oldest inactive datasets; iteration order is the publication
    // order, and kRetainLimit covers normal single-workbook usage.
    while (order_.size() > kRetainLimit) {
        QString oldest;
        for (const auto& candidate : order_) {
            if (candidate != activeId_) {
                oldest = candidate;
                break;
            }
        }
        if (oldest.isEmpty()) break;
        order_.removeAll(oldest);
        datasets_.remove(oldest);
    }
}

std::shared_ptr<const OsmxDataset> OsmxStore::active() const {
    QMutexLocker locker(&mutex_);
    const auto found = datasets_.constFind(activeId_);
    return found == datasets_.cend() ? std::shared_ptr<const OsmxDataset>() : found.value();
}

std::shared_ptr<const OsmxDataset> OsmxStore::find(const QString& datasetId) const {
    QMutexLocker locker(&mutex_);
    const auto found = datasets_.constFind(datasetId);
    return found == datasets_.cend() ? std::shared_ptr<const OsmxDataset>() : found.value();
}

QString OsmxStore::activeId() const {
    QMutexLocker locker(&mutex_);
    return activeId_;
}

QVector<QString> OsmxStore::datasetIds() const {
    QMutexLocker locker(&mutex_);
    return order_;
}

void OsmxStore::clear() {
    QMutexLocker locker(&mutex_);
    datasets_.clear();
    order_.clear();
    activeId_.clear();
}

} // namespace p1::data
