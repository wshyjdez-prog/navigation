#pragma once

#include "app/ImportTypes.h"
#include "data/osmx/OsmxDataset.h"
#include <QFutureWatcher>
#include <QObject>
#include <QThreadPool>
#include <atomic>

namespace p1::ui {

// Independent browsing worker. Uses the storage contract and leaves routing,
// endpoints and their cancellation tokens untouched. Only the newest view wins.
class MapRegionLoader final : public QObject {
    Q_OBJECT
public:
    explicit MapRegionLoader(QObject* parent=nullptr);
    ~MapRegionLoader() override;
    void setDataset(const app::ImportResult&, std::shared_ptr<const data::OsmxDataset>);
    void request(const data::OsmxBounds& visible);
    std::shared_ptr<const data::OsmxDataset> roads() const { return roads_; }
    quint64 revision() const { return published_; }
signals:
    void changed();
    void statusChanged(const QString& message);
private:
    struct Result {
        std::shared_ptr<const data::OsmxDataset> roads;
        data::OsmxBounds area;
        QString error;
    };
    void start();
    app::ImportResult dataset_;
    std::shared_ptr<const data::OsmxDataset> roads_;
    data::OsmxBounds loaded_, requested_;
    QThreadPool pool_;
    QFutureWatcher<Result>* watcher_=nullptr;
    std::shared_ptr<std::atomic_bool> cancellation_;
    quint64 generation_=0, published_=0;
    bool pending_=false;
};
}
