#include "MapPage.h"
#include "app/NavigationWorkflow.h"
#include "contracts/ImportedDataset.h"
#include "data/osmx/OsmxCache.h"
#include "ui/common/Components.h"
#include "ui/common/Theme.h"
#include <QtConcurrent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QTabWidget>
#include <QTimer>
#include <QElapsedTimer>
#include <QSaveFile>

namespace p1::ui {
namespace {
class Controls final : public QWidget {
    Q_OBJECT
public:
    Controls(Theme&,app::NavigationWorkflow* flow,QString directory):flow_(flow),directory_(std::move(directory)) {
        auto* l=new QVBoxLayout(this);l->setContentsMargins(0,0,0,0);l->setSpacing(12);
        auto* tabs=new QTabWidget;l->addWidget(tabs);
        auto* dataPage=new QWidget;auto* dl=new QVBoxLayout(dataPage);
        dl->addWidget(label("工作区域","sectionTitle"));
        datasets_=new QComboBox;datasets_->setObjectName("mapDatasetSelector");datasets_->setMinimumContentsLength(8);
        datasets_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        dl->addWidget(label("已导入的数据集","caption"));dl->addWidget(datasets_);
        auto* refresh=button("刷新数据集");refresh->setObjectName("refreshMapDatasets");dl->addWidget(refresh);
        auto* form=new QFormLayout;lat_=coordinate("regionLatitude",-85,85,30.593);lon_=coordinate("regionLongitude",-180,180,114.305);
        radius_=coordinate("regionRadiusKm",0.2,10,1);radius_->setDecimals(1);radius_->setSuffix(" km");radius_->setSingleStep(0.2);
        form->addRow("中心纬度",lat_);form->addRow("中心经度",lon_);form->addRow("工作半径",radius_);dl->addLayout(form);
        auto* load=button("加载真实地图",true);load->setObjectName("loadRoadNetwork");dl->addWidget(load);
        auto* note=label("标准 OSM 按区域加载完整道路；OSMX 加载整个已裁剪路网。规划与匹配限已加载范围。","caption");note->setWordWrap(true);dl->addWidget(note);dl->addStretch();tabs->addTab(dataPage,"数据");
        auto* routePage=new QWidget;auto* rl=new QVBoxLayout(routePage);auto* rf=new QFormLayout;
        startLat_=coordinate("routeStartLatitude",-90,90,30.593);startLon_=coordinate("routeStartLongitude",-180,180,114.305);
        endLat_=coordinate("routeEndLatitude",-90,90,30.597);endLon_=coordinate("routeEndLongitude",-180,180,114.309);
        rf->addRow("起点纬度",startLat_);rf->addRow("起点经度",startLon_);rf->addRow("终点纬度",endLat_);rf->addRow("终点经度",endLon_);rl->addLayout(rf);
        auto* pickStart=button("地图选起点");auto* pickEnd=button("地图选终点");pickStart->setObjectName("pickRouteStart");pickEnd->setObjectName("pickRouteEnd");rl->addWidget(pickStart);rl->addWidget(pickEnd);
        route_=button("计算 A* 路线",true);route_->setObjectName("calculateRoute");rl->addWidget(route_);
        auto* routeNote=label("WGS84 度；500 m 内吸附节点。通用路网距离最短，可包含步行/骑行道路；非汽车专用规则，不含转向限制与实时路况。","caption");routeNote->setWordWrap(true);rl->addWidget(routeNote);rl->addStretch();tabs->addTab(routePage,"路线");
        auto* matchPage=new QWidget;auto* ml=new QVBoxLayout(matchPage);
        auto* format=label("CSV 首行：latitude,longitude\n后续每行一对 WGS84 纬度、经度。支持 2–256 点；相邻点沿路距离不超过 2000 m。","secondary");format->setWordWrap(true);ml->addWidget(format);
        match_=button("选择轨迹并运行 FMM",true);match_->setObjectName("matchTrajectory");ml->addWidget(match_);
        auto* exportRoute=button("导出路线为测试轨迹");exportRoute->setObjectName("exportRouteTrajectory");ml->addWidget(exportRoute);
        auto* legend=label("灰线：原始轨迹\n橙线：FMM/HMM 匹配路径\n蓝线：A* 规划路线\n\n后台生成本区域 UBODT；取消会终止子进程，任务结束清理临时文件。","caption");legend->setWordWrap(true);ml->addWidget(legend);ml->addStretch();tabs->addTab(matchPage,"轨迹");
        auto* cancel=button("取消当前任务");cancel->setObjectName("cancelNavigationTask");l->addWidget(cancel);
        status_=label(flow_->status(),"secondary");status_->setObjectName("navigationStatus");status_->setWordWrap(true);l->addWidget(status_);
        metrics_=label({},"caption");metrics_->setObjectName("algorithmMetrics");metrics_->setWordWrap(true);l->addWidget(metrics_);
        auto* live=label({},"caption");live->setObjectName("workerStatus");live->setWordWrap(true);l->addWidget(live);
        auto* heartbeat=new QTimer(this);heartbeat->setInterval(100);auto clock=std::make_shared<QElapsedTimer>();clock->start();
        connect(heartbeat,&QTimer::timeout,this,[this,live,clock]{const auto gap=clock->restart();
            live->setText(QString("UI 心跳间隔 %1 ms · %2\n进程 RSS/HWM 含界面与渲染，非算法独占内存").arg(gap).arg(flow_->runtimeStatus()));});heartbeat->start();
        connect(refresh,&QPushButton::clicked,this,&Controls::refresh);
        connect(load,&QPushButton::clicked,this,[this]{int i=datasets_->currentIndex();if(i>=0&&i<entries_.size())flow_->load(entries_[i],lat_->value(),lon_->value(),radius_->value());});
        connect(route_,&QPushButton::clicked,this,[this]{flow_->plan({startLon_->value(),startLat_->value()},{endLon_->value(),endLat_->value()});});
        connect(match_,&QPushButton::clicked,this,[this]{const auto p=QFileDialog::getOpenFileName(this,"选择 WGS84 轨迹 CSV",{},"CSV (*.csv)");if(!p.isEmpty())flow_->match(p);});
        connect(exportRoute,&QPushButton::clicked,this,[this]{const auto& route=flow_->state().route;
            if(route.size()<2){status_->setText("请先计算路线，再导出沿路线生成的测试轨迹（非实采 GPS）");return;}
            const auto path=QFileDialog::getSaveFileName(this,"导出合成测试轨迹（非实采 GPS）","route-synthetic.csv","CSV (*.csv)");if(path.isEmpty())return;
            QByteArray csv("latitude,longitude\n");const int count=std::min(256,int(route.size()));
            for(int i=0;i<count;++i){const auto& p=route.at(qsizetype(i)*(route.size()-1)/(count-1));
                csv+=QString("%1,%2\n").arg(p.y(),0,'f',9).arg(p.x(),0,'f',9).toUtf8();}
            QSaveFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(csv)!=csv.size()||!file.commit())status_->setText("轨迹导出失败");
            else status_->setText(QString("已导出 %1 点合成测试轨迹；可选择此 CSV 检查 FMM。非实采 GPS。").arg(count));});
        connect(cancel,&QPushButton::clicked,flow_,&app::NavigationWorkflow::cancel);
        connect(pickStart,&QPushButton::clicked,this,[this]{flow_->beginMapPick(true);});
        connect(pickEnd,&QPushButton::clicked,this,[this]{flow_->beginMapPick(false);});
        connect(flow_,&app::NavigationWorkflow::endpointSelected,this,[this](bool start,QPointF p){
            (start?startLat_:endLat_)->setValue(p.y());(start?startLon_:endLon_)->setValue(p.x());});
        connect(flow_,&app::NavigationWorkflow::changed,this,[this,cancel,load]{status_->setText(flow_->status());metrics_->setText(flow_->metrics());
            route_->setEnabled(!flow_->busy()&&bool(flow_->state().network));match_->setEnabled(route_->isEnabled());cancel->setEnabled(flow_->busy());load->setEnabled(!flow_->busy()&&!entries_.isEmpty());});
        cancel->setEnabled(false);route_->setEnabled(false);match_->setEnabled(false);load->setEnabled(false);
        connect(this,&Controls::catalogReady,load,[load](bool ready){load->setEnabled(ready);});this->refresh();
    }
    ~Controls() override {if(catalog_)catalog_->waitForFinished();}
signals: void catalogReady(bool ready);
private:
    QDoubleSpinBox* coordinate(const char* name,double min,double max,double value){auto* b=new QDoubleSpinBox;b->setObjectName(name);b->setDecimals(7);b->setRange(min,max);b->setValue(value);b->setSingleStep(0.001);b->setButtonSymbols(QAbstractSpinBox::NoButtons);return b;}
    void refresh(){if(catalog_&&catalog_->isRunning())return;auto* w=new QFutureWatcher<QVector<app::ImportResult>>(this);catalog_=w;
        connect(w,&QFutureWatcher<QVector<app::ImportResult>>::finished,this,[this,w]{const auto previous=datasets_->currentData().toString();entries_=w->result();datasets_->clear();
            for(const auto& e:entries_)datasets_->addItem(e.fileName+" · "+e.sourceFormat,e.datasetId);
            const int i=datasets_->findData(previous);if(i>=0)datasets_->setCurrentIndex(i);
            if(entries_.isEmpty()) { status_->setText("请先在数据导入页导入文件"); }
            emit catalogReady(!entries_.isEmpty()&&!flow_->busy());catalog_=nullptr;w->deleteLater();});
        const auto dir=directory_;w->setFuture(QtConcurrent::run([dir]{QVector<app::ImportResult> all;const auto root=dir.isEmpty()?data::defaultImportCacheDirectory():dir;
            for(const auto& f:QDir(root).entryInfoList({"*.sqlite"},QDir::Files,QDir::Time))try{all.append(data::ImportedDataset(f.absoluteFilePath()).summary());}catch(const std::exception&){}
            all+=data::OsmxCache::list(dir.isEmpty()?data::OsmxCache::defaultDirectory():dir).entries;return all;}));}
    app::NavigationWorkflow* flow_;QString directory_;QComboBox* datasets_;QVector<app::ImportResult> entries_;
    QDoubleSpinBox *lat_,*lon_,*radius_,*startLat_,*startLon_,*endLat_,*endLon_;QLabel *status_,*metrics_;QPushButton *route_,*match_;
    QFutureWatcher<QVector<app::ImportResult>>* catalog_=nullptr;
};
}
QWidget* createAlgorithmsPage(Theme& theme,app::NavigationWorkflow* flow,const QString& cache,QWidget* parent){auto* page=new QWidget(parent);page->setObjectName("algorithmsPage");
    auto* l=new QVBoxLayout(page);l->setContentsMargins(0,0,0,0);l->setSpacing(12);l->addWidget(label("A* / FMM 算法工作台","sectionTitle"));
    auto* note=label("与地图页共享路网及结果。Boost.Graph A*（h=0）与固定版本 FMM/HMM 均在后台运行；完成后到地图页查看路径。","secondary");note->setWordWrap(true);l->addWidget(note);l->addWidget(scrollable(new Controls(theme,flow,cache)),1);return page;}
}
#include "MapPage.moc"
