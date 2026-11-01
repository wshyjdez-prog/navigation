#pragma once
// 4 号第 2 次作业：规划请求的异步对接桥（"对接真实计算"的落点）。
//
// 职责（落实 include/contracts/NavigationController.h 文件头 ① 的线程规则）：
//   - 把 NavigationController::routePlanRequested / routePlanCancelled（控制器线程，正式工程为 GUI 主线程）
//     经队列连接投递到位于内部工作线程的 RoutePlanner；
//   - 把 RoutePlanner::routeReady / routeFailed 经 Qt::QueuedConnection 回流到控制器线程，
//     再同步调用 controller->routeResultReady()/routeFailed()。
//   即：算法在后台线程跑，控制器/UI 线程永不阻塞（本次作业评分点"渲染和计算不阻塞 UI"）。
//
// 替换点：attachPlanner 接受任意 contracts::RoutePlanner 实现。本轮用 StubRoutePlanner；
// 3 号真实 A*/HMM 交付后替换传入对象即可，控制器与本桥零改动（契约既定策略）。
//
// 生命周期与取消：
//   - 桥取得 planner 所有权（析构时统一回收）；析构顺序 = 退出并等待工作线程 → 删除 planner，
//     保证析构后不再有任何回调（契约文件头 ②③）。
//   - 取消/新请求经同队列投递，与请求保持先来后到；停止后的残留结果由控制器按
//     契约文件头 ③④（silent/陈旧 requestId）丢弃——本桥只做透明转发，不代行状态机职责。
#include "contracts/NavigationController.h"

#include <QThread>
#include <atomic>

namespace p1::navigation {

class AsyncPlannerBridge : public QObject {
    Q_OBJECT
public:
    // controller 须位于当前线程（GUI 主线程）；桥不取得其所有权。
    explicit AsyncPlannerBridge(p1::contracts::NavigationController* controller, QObject* parent = nullptr);
    ~AsyncPlannerBridge() override;

    // 把 planner 移入内部工作线程并接通信号链；只能调用一次。
    // 之后 planner 的 requestRoute/cancel 均在工作线程事件循环中执行。
    void attachPlanner(p1::contracts::RoutePlanner* planner);

    // 在途请求数（仅控制器线程调用，测试/演示观察用）。
    // 计数在"请求发出 +1、结果回流或取消 -1"意义上近似；取消与迟到结果竞态时可能偏差 1，
    // 只用于演示与测试等待条件，不作为业务判断依据。
    std::uint64_t inFlightCount() const { return inFlight_.load(); }

signals:
    // 内部：经队列连接投递到工作线程执行。
    void planRequested(p1::contracts::RoutePlanRequest request);
    void planCancelled(std::uint64_t requestId);

private:
    p1::contracts::NavigationController* controller_ = nullptr;
    QThread worker_;
    p1::contracts::RoutePlanner* planner_ = nullptr; // 所有权归桥（析构时回收）
    std::atomic<std::uint64_t> inFlight_{0};
};

} // namespace p1::navigation
