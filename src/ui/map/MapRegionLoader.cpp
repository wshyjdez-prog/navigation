#include "MapRegionLoader.h"
#include "contracts/ImportedDataset.h"
#include <QtConcurrent>
#include <cmath>

namespace p1::ui {
namespace {
bool contains(const data::OsmxBounds& outer,const data::OsmxBounds& inner) {
    return outer.valid&&inner.valid&&outer.minLat<=inner.minLat&&outer.minLon<=inner.minLon&&
        outer.maxLat>=inner.maxLat&&outer.maxLon>=inner.maxLon;
}
}
MapRegionLoader::MapRegionLoader(QObject* parent):QObject(parent) { pool_.setMaxThreadCount(1); }
MapRegionLoader::~MapRegionLoader() {
    if(cancellation_) cancellation_->store(true);
    pool_.waitForDone();
}
void MapRegionLoader::setDataset(const app::ImportResult& entry,std::shared_ptr<const data::OsmxDataset> roads) {
    if(cancellation_) cancellation_->store(true);
    ++generation_;pending_=false;dataset_=entry;roads_=std::move(roads);loaded_={};++published_;
    emit changed();
}
void MapRegionLoader::request(const data::OsmxBounds& visible) {
    if(dataset_.cachePath.isEmpty()||dataset_.sourceFormat=="osmx") return;
    if(!visible.valid||!std::isfinite(visible.minLat)||!std::isfinite(visible.maxLat)||
        !std::isfinite(visible.minLon)||!std::isfinite(visible.maxLon)) return;
    if(contains(loaded_,visible)) {
        // Returning to the cached view also invalidates any in-flight request.
        ++generation_;pending_=false;if(cancellation_) cancellation_->store(true);
        emit statusChanged(roads_&&roads_->wayCount()?QString():QString("当前区域没有已导入道路"));return;
    }
    auto area=visible;
    const double dy=(area.maxLat-area.minLat)*.12,dx=(area.maxLon-area.minLon)*.12;
    area.minLat=std::max(-85.,area.minLat-dy);area.maxLat=std::min(85.,area.maxLat+dy);
    area.minLon=std::max(-180.,area.minLon-dx);area.maxLon=std::min(180.,area.maxLon+dx);
    ++generation_;if(cancellation_) cancellation_->store(true);pending_=false;
    if(area.minLat>=area.maxLat||area.minLon>=area.maxLon||area.maxLat-area.minLat>.3||area.maxLon-area.minLon>.5) {
        emit statusChanged("请放大地图以加载道路");return;
    }
    requested_=area;pending_=true;
    emit statusChanged("正在加载当前区域…");
    if(!watcher_) start();
}
void MapRegionLoader::start() {
    pending_=false;
    const auto generation=generation_;const auto entry=dataset_;const auto area=requested_;
    auto token=std::make_shared<std::atomic_bool>(false);cancellation_=token;
    auto* watcher=new QFutureWatcher<Result>(this);watcher_=watcher;
    connect(watcher,&QFutureWatcher<Result>::finished,this,[this,watcher,generation,token] {
        const auto result=watcher->result();watcher_=nullptr;watcher->deleteLater();
        if(generation==generation_&&!token->load()) {
            if(result.error.isEmpty()) {
                roads_=result.roads;loaded_=result.area;++published_;
                emit statusChanged(roads_->wayCount()?QString():QString("当前区域没有已导入道路"));
                emit changed();
            } else emit statusChanged("区域加载失败："+result.error+"；移动地图可重试");
        }
        if(pending_) start();
    });
    watcher->setFuture(QtConcurrent::run(&pool_,[entry,area,token] {
        Result result;result.area=area;
        try {result.roads=data::ImportedDataset(entry.cachePath).roads(area,token,true);}
        catch(const std::exception& e) {result.error=QString::fromUtf8(e.what());}
        return result;
    }));
}
}
