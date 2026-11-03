#pragma once
#include "contracts/MapGeometry.h"
#include <QImage>
#include <QObject>
#include <QRectF>
#include <QSize>
#include <QThreadPool>
#include <QFutureWatcher>
namespace p1::ui {
// Web Mercator world coordinates, in metres. Routing retains its own projection.
QPointF mapProject(QPointF geo);
QPointF mapUnproject(QPointF metres);
struct PyramidStats { int zoom=0, tiles=0, hits=0, parents=0, roads=0; };
class TileBackend;
class TilePyramid final:public QObject {
    Q_OBJECT
public:
    explicit TilePyramid(QObject* parent=nullptr,QString cacheRoot={});
    ~TilePyramid() override;
    void setDataset(const app::ImportResult&,std::shared_ptr<const data::OsmxDataset>);
    void request(QRectF worldView,QSize size,bool dark);
signals:
    void frameReady(QImage image,QRectF view,p1::ui::PyramidStats stats);
    void statusChanged(QString text);
private:
    void start();
    QThreadPool pool_;
    std::shared_ptr<TileBackend> backend_;
    QString root_;
    QFutureWatcher<QString>* watcher_=nullptr;
    std::shared_ptr<std::atomic_bool> cancel_;
    QRectF view_;QSize size_;bool dark_=false,pending_=false;
    quint64 generation_=0;
};
}
Q_DECLARE_METATYPE(p1::ui::PyramidStats)
