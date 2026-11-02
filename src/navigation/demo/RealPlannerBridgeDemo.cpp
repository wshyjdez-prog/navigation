// 4 号：真实规划器对接演示（3 号 BoostRoutePlanner + 4 号 AsyncPlannerBridge）。
//
// 证明"契约先行"的既定策略成立：桥与控制器零改动，仅把 StubRoutePlanner 换成
// BoostRoutePlanner（contracts::RoutePlanner 即插即换），四条链路全部正确回流：
//   1. 正常规划：start → 后台 A* → routeReady → 采纳（Navigating，source=Real）；
//   2. stop() 取消：残留结果被控制器按契约 ③ 静默丢弃；
//   3. 不可达终点：routeFailed(NoRouteAvailable) → 导航中止；
//   4. 非节点端点：3 号 locateNode 精确匹配失败 → routeFailed(PlanningFailed) → 中止。
//
// 样例路网（member3-astar-synthetic v1，米制，节点 id→坐标）：
//   900(0,0) → 42(3,0) → 5(6,0)，另有 701(0,5)、777(99,99 无边)。
//   最优解 900→42→5：cost 3+3=6 m（直连边 900→5 cost 9 更差）。
// 投影原点取 WGS84 (60°N, 116°E)；节点坐标经等距圆柱投影往返（误差 ~1e-13 m，
// 远小于 3 号 locateNode 的 1e-6 m 吸附容差）。
//
// 本程序退出码 0 即演示成功；注册为 ctest navigation-real-planner-demo
// （P1_NAVIGATION_REAL_PLANNER=ON 时参与构建）。
#include "../AsyncPlannerBridge.h"
#include "../GeoProjector.h"
#include "../RecordingController.h"

#include "model/Types.h"
#include "qt/BoostRoutePlanner.h"
#include "samples/FixedRoadNetwork.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDateTime>
#include <QEventLoop>
#include <QThread>
#include <QTimer>
#include <QtMath>

#include <cmath>
#include <cstdio>
#include <functional>

using namespace p1::navigation;
using p1::contracts::NavigationEventKind;

namespace {

// 样例图定义于米制原点 (0,0)；投影原点取北京附近纬度带作演示。
constexpr double kOriginLatDeg = 60.0;
constexpr double kOriginLonDeg = 116.0;
constexpr double kMetresPerDegLat = 111320.0;
// 与纬度 60° 带一致的东西向比例（同值参与正逆两个方向，保证往返确定性）。
const double kMetresPerDegLon =
    kMetresPerDegLat * std::cos(qDegreesToRadians(kOriginLatDeg));

GeoProjector makeProjector() {
    return GeoProjector(kOriginLatDeg, kOriginLonDeg, kMetresPerDegLat, kMetresPerDegLon);
}

// 把样例图节点坐标逆投影为 WGS84（演示输入构造；误差 ~1e-13 m，可被 1e-6 m 容差吸附）。
p1::contracts::GeoCoordinate nodeCoordinate(double xM, double yM) {
    p1::contracts::GeoCoordinate c;
    c.crs = p1::contracts::CoordinateSystem::WGS84;
    c.latitudeDeg = kOriginLatDeg + yM / kMetresPerDegLat;
    c.longitudeDeg = kOriginLonDeg + xM / kMetresPerDegLon;
    return c;
}

// dataVersion 必须与图的 metadata.version 完全一致（3 号 requestRoute 硬性校验），
// requestId 由调用方保证单调递增且 >0（3 号 requestRoute 硬性校验）。
p1::contracts::RoutePlanRequest makeRequest(std::uint64_t requestId,
                                            const p1::contracts::GeoCoordinate& from,
                                            const p1::contracts::GeoCoordinate& to) {
    p1::contracts::RoutePlanRequest request;
    request.requestId = requestId;
    request.issuedAtMs = QDateTime::currentMSecsSinceEpoch();
    request.origin = from;
    request.destination = to;
    request.dataVersion = QStringLiteral("v1");
    return request;
}

void printEvent(const p1::contracts::NavigationEvent& event) {
    std::printf("  [event] %-20s status=%-10s req=%llu nav=%llu reason=%s\n",
                p1::contracts::toString(event.kind), p1::contracts::toString(event.status),
                static_cast<unsigned long long>(event.requestId),
                static_cast<unsigned long long>(event.navigationId), qPrintable(event.reason));
}

bool waitFor(const std::function<bool()>& condition, int timeoutMs) {
    QDeadlineTimer deadline(timeoutMs);
    while (!condition() && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return condition();
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    Q_UNUSED(argc);
    Q_UNUSED(argv);

    std::printf("=== P1 real planner bridge demo (member-3 BoostRoutePlanner, sample graph v1) ===\n");

    const auto graphData = p1::algorithms::samples::fixedRoadNetwork();
    const auto publication = p1::algorithms::model::publishGraph(graphData);
    if (!publication.validation.ok() || !publication.snapshot) {
        std::printf("FAIL: sample graph rejected by member-3 validation (%zu error(s))\n",
                    publication.validation.errors.size());
        return 1;
    }

    RecordingController controller;
    AsyncPlannerBridge bridge(&controller);
    // 所有权移交桥（析构统一回收）；契约既定策略：桥与控制器零改动，仅换规划器实现。
    auto* planner = new p1::algorithms::BoostRoutePlanner(
        publication.snapshot, makeProjector(), /*assumedSpeedMps=*/8.0);
    bridge.attachPlanner(planner);

    int appliedCount = 0;
    QObject::connect(&controller, &p1::contracts::NavigationController::navigationEventOccurred,
                     [&](const p1::contracts::NavigationEvent& event) {
                         printEvent(event);
                         if (event.kind == NavigationEventKind::RoutePlanApplied) {
                             ++appliedCount;
                         }
                     });

    // ── 阶段 1：正常规划 —— A* 在后台线程执行，结果经队列连接回流并采纳 ──
    std::printf("--- phase 1: real A* plan 900 -> 5 (expected distance 6.00 m, route bgl:*) ---\n");
    const auto node900 = nodeCoordinate(0.0, 0.0);
    const auto node5 = nodeCoordinate(6.0, 0.0);
    const std::uint64_t id1 = controller.start(makeRequest(1, node900, node5));
    if (id1 != 1) {
        std::printf("FAIL: start rejected (id=%llu)\n", static_cast<unsigned long long>(id1));
        return 1;
    }
    if (!waitFor([&] { return controller.status() != p1::contracts::NavigationStatus::Planning; }, 10'000)) {
        std::printf("FAIL: plan did not finish within 10 s\n");
        return 1;
    }
    if (controller.status() != p1::contracts::NavigationStatus::Navigating || appliedCount != 1) {
        std::printf("FAIL: route not applied (status=%s)\n", p1::contracts::toString(controller.status()));
        return 1;
    }
    const auto snapshot = controller.snapshot();
    if (snapshot.route->source != p1::contracts::SourceKind::Real) {
        std::printf("FAIL: route source is not Real\n");
        return 1;
    }
    if (!snapshot.route->routeId.startsWith(QStringLiteral("bgl:"))) {
        std::printf("FAIL: unexpected routeId %s\n", qPrintable(snapshot.route->routeId));
        return 1;
    }
    if (!(std::fabs(snapshot.route->totalDistanceM - 6.0) < 1e-6)) {
        std::printf("FAIL: distance %.4f m != 6.00 m\n", snapshot.route->totalDistanceM);
        return 1;
    }
    std::printf("  applied: route=%s distance=%.2f m duration=%.2f s (source=Real)\n",
                qPrintable(snapshot.route->routeId), snapshot.route->totalDistanceM,
                snapshot.route->totalDurationSec);

    // ── 阶段 2：stop() 取消 —— 残留结果（若有）被契约 ③ 静默丢弃 ──
    std::printf("--- phase 2: stop() cancels in-flight; no applied/aborted events afterwards ---\n");
    controller.stop(); // 回 Idle、silent；进入 Navigating 后 stop 合法
    const auto node42 = nodeCoordinate(3.0, 0.0);
    const std::uint64_t id2 = controller.start(makeRequest(2, node900, node42));
    if (id2 != 2) {
        std::printf("FAIL: second start rejected\n");
        return 1;
    }
    controller.stop(); // 立即停止（A* 在 5 节点图上极快，结果若抢在取消前完成，
                       // 也必须被控制器静默丢弃——两种竞态结果都应表现为"零新事件"）
    const int eventsAtStop = static_cast<int>(controller.events().size());
    const QDeadlineTimer settle(300); // 给足"若未被取消本应回流"的时间
    while (!settle.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    const int eventsAfter = static_cast<int>(controller.events().size());
    if (eventsAfter != eventsAtStop || controller.status() != p1::contracts::NavigationStatus::Idle) {
        std::printf("FAIL: %d unexpected event(s) after stop()\n", eventsAfter - eventsAtStop);
        return 1;
    }
    std::printf("  silent after stop: %d events before == %d after (PASS)\n", eventsAtStop, eventsAfter);

    // ── 阶段 3：不可达终点（节点 777 无出边）→ routeFailed(NoRouteAvailable) ──
    std::printf("--- phase 3: unreachable destination (node 777) -> NavigationAborted ---\n");
    int failureCount = 0;
    p1::contracts::NavigationErrorCode lastError{};
    QObject::connect(&controller, &p1::contracts::NavigationController::navigationErrorOccurred,
                     [&](const p1::contracts::NavigationError& error) {
                         ++failureCount;
                         lastError = error.code;
                         std::printf("  [error] %s: %s\n", p1::contracts::toString(error.code),
                                     qPrintable(error.message));
                     });
    controller.start(makeRequest(3, node900, nodeCoordinate(99.0, 99.0)));
    if (!waitFor([&] { return controller.status() == p1::contracts::NavigationStatus::Idle; }, 10'000)) {
        std::printf("FAIL: unreachable case did not settle to Idle\n");
        return 1;
    }
    if (failureCount != 1 || lastError != p1::contracts::NavigationErrorCode::NoRouteAvailable) {
        std::printf("FAIL: expected exactly one NoRouteAvailable error\n");
        return 1;
    }
    std::printf("  unreachable -> NoRouteAvailable -> aborted (PASS)\n");

    // ── 阶段 4：非节点端点（投影不落在任何节点上）→ routeFailed(PlanningFailed) ──
    std::printf("--- phase 4: endpoint not on a graph node -> PlanningFailed -> aborted ---\n");
    controller.start(makeRequest(4, node900, nodeCoordinate(3.0, 2.5))); // (3,2.5) 不是节点
    if (!waitFor([&] { return controller.status() == p1::contracts::NavigationStatus::Idle; }, 10'000)) {
        std::printf("FAIL: invalid-node case did not settle to Idle\n");
        return 1;
    }
    if (failureCount != 2 || lastError != p1::contracts::NavigationErrorCode::PlanningFailed) {
        std::printf("FAIL: expected exactly one PlanningFailed error\n");
        return 1;
    }
    std::printf("  non-node endpoint -> PlanningFailed -> aborted (PASS)\n");

    std::printf("=== real planner demo OK ===\n");
    return 0;
}
