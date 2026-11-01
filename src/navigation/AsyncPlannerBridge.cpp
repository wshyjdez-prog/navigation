#include "AsyncPlannerBridge.h"

#include <QtGlobal>

namespace p1::navigation {

AsyncPlannerBridge::AsyncPlannerBridge(p1::contracts::NavigationController* controller, QObject* parent)
    : QObject(parent), controller_(controller) {
    Q_ASSERT(controller_ != nullptr);
    // 跨线程队列连接需要自定义类型注册；重复调用无害（NavigationTypes.h 既定入口）。
    p1::contracts::registerNavigationMetaTypes();
}

AsyncPlannerBridge::~AsyncPlannerBridge() {
    // 先退出工作线程事件循环并等待，保证删除 planner 时无并发访问；
    // 工作线程中尚未执行的排队任务随退出被丢弃，符合契约"析构后不再回调"。
    worker_.quit();
    worker_.wait();
    delete planner_;
    planner_ = nullptr;
}

void AsyncPlannerBridge::attachPlanner(p1::contracts::RoutePlanner* planner) {
    Q_ASSERT(planner != nullptr);
    Q_ASSERT(planner_ == nullptr); // 只允许接入一次
    planner_ = planner;
    planner_->moveToThread(&worker_);
    worker_.start();

    // 控制器请求 → 计数 + 投递工作线程（同一信号直连本对象，顺序即发出顺序）。
    connect(controller_, &p1::contracts::NavigationController::routePlanRequested, this,
            [this](const p1::contracts::RoutePlanRequest& request) {
                ++inFlight_;
                emit planRequested(request);
            });

    // 控制器取消（stop()/重规划）→ 工作线程；在途计数同步回收（取消的请求不会再回流结果）。
    connect(controller_, &p1::contracts::NavigationController::routePlanCancelled, this,
            &AsyncPlannerBridge::planCancelled, Qt::QueuedConnection);
    connect(this, &AsyncPlannerBridge::planCancelled, planner_,
            [this, planner](std::uint64_t requestId) {
                if (inFlight_.load() > 0) {
                    --inFlight_;
                }
                planner->cancel(requestId);
            },
            Qt::QueuedConnection);

    // 投递工作线程执行：functor 以 planner 为 context（其线程 = 工作线程），
    // Qt::QueuedConnection 保证 lambda 在工作线程事件循环中运行，不占控制器线程。
    connect(this, &AsyncPlannerBridge::planRequested, planner_,
            [planner](const p1::contracts::RoutePlanRequest& request) { planner->requestRoute(request); },
            Qt::QueuedConnection);

    // 结果回流：planner 在工作线程发出 → 经队列连接回到本桥（控制器线程）→ 同步转调控制器。
    // 契约：信号参数按值复制；转发为透明透传，采纳/丢弃（silent、陈旧 requestId）由控制器实现。
    connect(planner_, &p1::contracts::RoutePlanner::routeReady, this,
            [this](const p1::contracts::RoutePlanResult& result) {
                if (inFlight_.load() > 0) {
                    --inFlight_;
                }
                controller_->routeResultReady(result);
            },
            Qt::QueuedConnection);
    connect(planner_, &p1::contracts::RoutePlanner::routeFailed, this,
            [this](std::uint64_t requestId, p1::contracts::NavigationErrorCode code, const QString& message) {
                if (inFlight_.load() > 0) {
                    --inFlight_;
                }
                controller_->routeFailed(requestId, code, message);
            },
            Qt::QueuedConnection);
}

} // namespace p1::navigation
