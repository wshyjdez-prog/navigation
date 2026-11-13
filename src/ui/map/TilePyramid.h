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
// Area names are baked into each XYZ tile (DEC-20261111-1-area-labels-01).
// complete: the last frame of a request; earlier ones are progressive.
// blank: tiles in the visible view left without any image in this frame.
struct PyramidStats { int zoom=0, tiles=0, hits=0, parents=0, roads=0, blank=0; bool complete=false; };
// Map clarity = tile render density. Tiles keep their XYZ level and logical
// 256 px layout but are rasterised at 256/384/512 px (1x/1.5x/2x), so the
// screen shrinks a detailed tile instead of enlarging a coarse one. Text and
// line sizes stay the same on screen; generation and cache cost grow by d^2.
enum class MapClarity { Low, Medium, High };
double clarityDensity(MapClarity clarity);
// Edge loading (DEC-20261111-1-edge-loading-01): frames cover 256 px beyond
// each edge of the request, coarse z-2 tiles fill gaps first and frames are
// republished progressively. frameReady's view is the area the image covers.
class TileBackend;
class TilePyramid final:public QObject {
    Q_OBJECT
public:
    explicit TilePyramid(QObject* parent=nullptr,QString cacheRoot={});
    ~TilePyramid() override;
    void setDataset(const app::ImportResult&,std::shared_ptr<const data::OsmxDataset>);
    void request(QRectF worldView,QSize size,bool dark);
    // Switches the raster namespace; the geometry index is reused.
    void setClarity(MapClarity clarity);
    MapClarity clarity() const { return clarity_; }
signals:
    void frameReady(QImage image,QRectF view,p1::ui::PyramidStats stats);
    void statusChanged(QString text);
private:
    void start();
    QThreadPool pool_;
    std::shared_ptr<TileBackend> backend_;
    QString root_;
    MapClarity clarity_=MapClarity::Low;
    app::ImportResult entry_;std::shared_ptr<const data::OsmxDataset> dataset_;bool hasDataset_=false;
    QFutureWatcher<QString>* watcher_=nullptr;
    std::shared_ptr<std::atomic_bool> cancel_;
    QRectF view_;QSize size_;bool dark_=false,pending_=false;
    quint64 generation_=0;
};
}
Q_DECLARE_METATYPE(p1::ui::PyramidStats)
