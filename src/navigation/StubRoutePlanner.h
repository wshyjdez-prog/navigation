#pragma once
// 4 号第 2 次作业：确定性路线规划桩（实现 include/contracts/NavigationController.h 中的
// contracts::RoutePlanner 调用形状）。替换点说明见该文件"3 号 RoutePlanner 的调用形状"注释：
// 3 号真实 A*/HMM 交付后整体替换本类，NavigationController 与 AsyncPlannerBridge 零改动。
//
// 特性（均为验证对接层服务，不代表真实算法能力）：
//   - 延迟可注入：setComputeDelayMs 模拟真实计算耗时，用于"不阻塞 UI"验证与演示。
//   - 失败可注入：setFailNextPlan 触发一次 routeFailed，用于失败回流链路验证。
//   - 取消语义：cancel(requestId) 后不再回吐该请求的任何结果（契约硬性规则）；
//     setHonorCancel(false) 可模拟"取消不及时"的规划器，用于陈旧结果拒绝链路验证。
//   - 输出确定性：同一请求永远得到相同 geometryRef/距离/引导，无随机数，期望值可手工复算。
//
// 线程归属：本对象须位于工作线程（由 AsyncPlannerBridge::attachPlanner 移入）；
// requestRoute/cancel 只能在该线程调用（AsyncPlannerBridge 已保证经队列连接进入）。
//
// 距离说明：stub 用 WGS84 平均半径 haversine 自算 totalDistanceM，仅供演示与测试比对，
// 不代表 3 号真实规划结果；请求输入仍是带坐标系标记的度（NavigationTypes.h 文件头 ⑦
// 的度/米边界不变——stub 是"假想的 3 号"，在其内部自算距离与 3 号适配层的职责对应）。
#include "contracts/NavigationController.h"

#include <QTimer>
#include <map>

namespace p1::navigation {

class StubRoutePlanner final : public p1::contracts::RoutePlanner {
    Q_OBJECT
public:
    explicit StubRoutePlanner(QObject* parent = nullptr);

    // 模拟计算耗时（毫秒）。默认 0（下一个事件循环周期完成）。
    // 仅允许在对象进入工作线程前（构造后、attachPlanner 前）调用，无锁。
    void setComputeDelayMs(int delayMs) { computeDelayMs_ = delayMs; }
    // 注入下一次规划失败：下一次 finishPlan 走 routeFailed（随后自动复位）。
    void setFailNextPlan(
        bool fail,
        p1::contracts::NavigationErrorCode code = p1::contracts::NavigationErrorCode::PlanningFailed) {
        failNextPlan_ = fail;
        failCode_ = code;
    }
    // 故障注入：设为 false 后 cancel 只记录不拦截，迟到的结果照常回流，
    // 用于验证消费方（控制器）的陈旧结果拒绝链路。默认 true（遵守契约）。
    void setHonorCancel(bool honor) { honorCancel_ = honor; }

    // ── contracts::RoutePlanner（在工作线程执行） ────────────────────────────
    std::uint64_t requestRoute(const p1::contracts::RoutePlanRequest& request) override;
    void cancel(std::uint64_t requestId) override;

signals:
    // 测试观察点：stub 实际完成一次规划（cancel 拦截的不发）。
    void planExecuted(std::uint64_t requestId);

private slots:
    void finishPlan(std::uint64_t requestId);

private:
    static p1::contracts::RoutePlanResult buildResult(const p1::contracts::RoutePlanRequest& request);
    static double haversineM(const p1::contracts::GeoCoordinate& a, const p1::contracts::GeoCoordinate& b);

    int computeDelayMs_ = 0;
    bool failNextPlan_ = false;
    bool honorCancel_ = true;
    p1::contracts::NavigationErrorCode failCode_ = p1::contracts::NavigationErrorCode::PlanningFailed;
    std::map<std::uint64_t, p1::contracts::RoutePlanRequest> pending_; // 待完成请求（工作线程私有）
    std::map<std::uint64_t, bool> cancelled_;                          // 已取消请求：拦截其结果回流
    std::uint64_t generatedRequestId_ = 0;                             // request.requestId==0 时自造
};

} // namespace p1::navigation
