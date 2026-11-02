// 4 号：真实规划器（3 号 BoostRoutePlanner）对接集成测试。
// 覆盖条款（对应 AGENTS.md Stage5 标注要求）：
//   1. projectorRoundTripAndRejection —— 投影器确定性往返（误差 << 1e-6 m 吸附容差）与非法输入拒绝；
//   2. realPlannerAppliesRoute —— 端到端：真实 A* 结果经桥回流并被采纳（source=Real、距离 6 m、bgl: 路由 ID、
//      回流线程归属 == 控制器线程，契约文件头 ①③）；
//   3. latestRequestSupersedesPending —— 规划器"最新请求生效"语义：在途请求被取消后零回调（契约 ④）；
//   4. dataVersionMismatchYieldsSilence —— dataVersion 不匹配被拒收（requestId=0），锁定现状行为；
//   5. unreachableDestinationFails —— Unreachable → NoRouteAvailable → 导航中止；
//   6. nonNodeEndpointFails —— 端点投影不落在图节点 → PlanningFailed → 中止；
//   7. destructorIsSilent —— 桥析构后不再有任何回调（契约 ②③）。
// 注：控制器层"Planning 中重新 start"的采纳/取消由 PlannerBridgeTests（stub 可注入延迟）覆盖，
//     真实规划器计算过快无法确定性构造该竞态，故第 3 条在规划器层直接验证。
#include "../AsyncPlannerBridge.h"
#include "../GeoProjector.h"
#include "../RecordingController.h"

#include "model/Types.h"
#include "qt/BoostRoutePlanner.h"
#include "samples/FixedRoadNetwork.h"

#include <QtTest>

#include <cmath>
#include <functional>
#include <memory>

using namespace p1::navigation;
using p1::contracts::NavigationEventKind;
using p1::contracts::NavigationErrorCode;
using p1::contracts::NavigationStatus;

namespace {

// 与演示程序一致：样例图米制原点 → WGS84 投影原点 (60°N, 116°E)。
constexpr double kOriginLatDeg = 60.0;
constexpr double kOriginLonDeg = 116.0;
constexpr double kMetresPerDegLat = 111320.0;
const double kMetresPerDegLon = kMetresPerDegLat * std::cos(qDegreesToRadians(kOriginLatDeg));

GeoProjector makeProjector() {
    return GeoProjector(kOriginLatDeg, kOriginLonDeg, kMetresPerDegLat, kMetresPerDegLon);
}

p1::contracts::GeoCoordinate nodeCoordinate(double xM, double yM) {
    p1::contracts::GeoCoordinate c;
    c.crs = p1::contracts::CoordinateSystem::WGS84;
    c.latitudeDeg = kOriginLatDeg + yM / kMetresPerDegLat;
    c.longitudeDeg = kOriginLonDeg + xM / kMetresPerDegLon;
    return c;
}

p1::contracts::RoutePlanRequest makeRequest(std::uint64_t requestId,
                                            const p1::contracts::GeoCoordinate& from,
                                            const p1::contracts::GeoCoordinate& to,
                                            const QString& dataVersion = QStringLiteral("v1")) {
    p1::contracts::RoutePlanRequest request;
    request.requestId = requestId;
    request.issuedAtMs = QDateTime::currentMSecsSinceEpoch();
    request.origin = from;
    request.destination = to;
    request.dataVersion = dataVersion;
    return request;
}

// 与图发布联动的小型装配：controller → bridge → BoostRoutePlanner（样例图 v1）。
// 成员声明顺序 = 析构顺序的倒序：bridge 先于 controller 析构（桥先退线程再回收 planner）。
struct Rig {
    p1::algorithms::model::GraphPublication publication;
    std::unique_ptr<RecordingController> controller;
    std::unique_ptr<AsyncPlannerBridge> bridge;

    Rig() {
        publication = p1::algorithms::model::publishGraph(p1::algorithms::samples::fixedRoadNetwork());
        Q_ASSERT(publication.validation.ok() && publication.snapshot);
        controller = std::make_unique<RecordingController>();
        bridge = std::make_unique<AsyncPlannerBridge>(controller.get());
        bridge->attachPlanner(
            new p1::algorithms::BoostRoutePlanner(publication.snapshot, makeProjector(), 8.0));
    }
};

bool waitFor(const std::function<bool()>& condition, int timeoutMs = 10'000) {
    QDeadlineTimer deadline(timeoutMs);
    while (!condition() && !deadline.hasExpired()) {
        QTest::qWait(5);
    }
    return condition();
}

const p1::contracts::GeoCoordinate node900Coord = nodeCoordinate(0.0, 0.0); // 样例节点 900 (0,0)

} // namespace

class RealPlannerBridgeTests final : public QObject {
    Q_OBJECT

private slots:
    // 1. 投影器：已知节点往返 + 非法输入拒绝（纯单元，不依赖规划器）。
    void projectorRoundTripAndRejection() {
        const auto projector = makeProjector();
        // 样例图全部 5 个节点：往返误差必须远小于 3 号 locateNode 的 1e-6 m 吸附容差。
        const std::pair<double, double> nodes[] = {{0.0, 0.0}, {3.0, 0.0}, {6.0, 0.0},
                                                   {0.0, 5.0}, {99.0, 99.0}};
        for (const auto& [x, y] : nodes) {
            const auto point = projector(nodeCoordinate(x, y));
            QVERIFY(point.has_value());
            QVERIFY(std::fabs(point->x_m - x) < 1e-9);
            QVERIFY(std::fabs(point->y_m - y) < 1e-9);
        }
        // 非法输入：非 WGS84 坐标系（契约枚举仅 WGS84/GCJ02/BD09）、NaN/Inf → nullopt
        //（交由 3 号按 InvalidNode 处理）。
        p1::contracts::GeoCoordinate bad = nodeCoordinate(1.0, 1.0);
        bad.crs = p1::contracts::CoordinateSystem::GCJ02;
        QVERIFY(!projector(bad).has_value());
        bad = nodeCoordinate(1.0, 1.0);
        bad.latitudeDeg = std::nan("");
        QVERIFY(!projector(bad).has_value());
    }

    // 2. 端到端成功：真实 A* 结果采纳，契约字段贯通。
    void realPlannerAppliesRoute() {
        Rig rig;
        const auto node5 = nodeCoordinate(6.0, 0.0);
        const auto id = rig.controller->start(makeRequest(1, node900Coord, node5));
        QCOMPARE(id, 1ULL);
        QVERIFY(waitFor([&] { return rig.controller->status() == NavigationStatus::Navigating; }));
        QVERIFY(rig.controller->lastResultOnControllerThread()); // Qt::QueuedConnection 回流证明

        const auto snapshot = rig.controller->snapshot();
        QVERIFY(snapshot.route.has_value());
        QCOMPARE(snapshot.route->requestId, 1ULL);
        QCOMPARE(snapshot.route->source, p1::contracts::SourceKind::Real);
        QVERIFY(snapshot.route->routeId.startsWith(QStringLiteral("bgl:")));
        // 最优解 900→42→5：cost 3+3=6，直连 900→5 cost 9 更差（A* 以 cost 为目标）。
        QVERIFY(std::fabs(snapshot.route->totalDistanceM - 6.0) < 1e-6);
        QVERIFY(snapshot.route->totalDurationSec > 0.0);
        // 事件序：Started → PlanningStarted → Applied（无失败/拒绝）。
        const auto& events = rig.controller->events();
        QVERIFY(hasEvent(events, NavigationEventKind::RoutePlanApplied));
        QVERIFY(!hasEvent(events, NavigationEventKind::RoutePlanRejected));
        QVERIFY(!hasEvent(events, NavigationEventKind::NavigationAborted));
    }

    // 3. 规划器"最新请求生效"：请求 2 使在途请求 1 被取消，1 的结果零回调。
    //    规划器置于调用线程直接驱动（不经桥），使时序确定性；经桥的取消链路
    //    已由 PlannerBridgeTests（stub 注入延迟）与真实演示程序覆盖。
    void latestRequestSupersedesPending() {
        p1::algorithms::model::GraphPublication publication =
            p1::algorithms::model::publishGraph(p1::algorithms::samples::fixedRoadNetwork());
        p1::algorithms::BoostRoutePlanner planner(publication.snapshot, makeProjector(), 8.0);

        const auto node5 = nodeCoordinate(6.0, 0.0);
        QCOMPARE(planner.requestRoute(makeRequest(1, node900Coord, node5)), 1ULL);
        QCOMPARE(planner.requestRoute(makeRequest(2, node900Coord, node5)), 2ULL); // 取消在途 1
        int readyCount = 0;
        std::uint64_t readyId = 0;
        int failedCount = 0;
        QObject::connect(&planner, &p1::contracts::RoutePlanner::routeReady,
                         [&](const p1::contracts::RoutePlanResult& result) {
                             ++readyCount;
                             readyId = result.requestId;
                         });
        QObject::connect(&planner, &p1::contracts::RoutePlanner::routeFailed,
                         [&](std::uint64_t, NavigationErrorCode, const QString&) { ++failedCount; });
        QVERIFY(waitFor([&] { return readyCount > 0 || failedCount > 0; }));

        QCOMPARE(readyCount, 1); // 只回最新请求
        QCOMPARE(readyId, 2ULL);
        QCOMPARE(failedCount, 0); // 被取消的请求 1 静默（契约：取消后不得回吐结果）
    }

    // 4. dataVersion 不匹配：3 号 requestRoute 拒收（返回 0）→ 永无回调。
    //    锁定现状行为：桥透明转发不代行兜底，接入 UI 后需 1 号侧规划超时兜底（已在对接方案中记录）。
    void dataVersionMismatchYieldsSilence() {
        Rig rig;
        const auto node5 = nodeCoordinate(6.0, 0.0);
        const auto id = rig.controller->start(
            makeRequest(1, node900Coord, node5, QStringLiteral("wrong-version")));
        QCOMPARE(id, 1ULL); // 控制器受理（其校验与规划器校验相互独立）
        QTest::qWait(300);
        QCOMPARE(rig.controller->status(), NavigationStatus::Planning); // 仍挂起：无结果回流
        // 零结果事件：成功/失败/拒绝皆无（start 本身的 Started/PlanningStarted 记录除外）。
        const auto& events = rig.controller->events();
        QVERIFY(!hasEvent(events, NavigationEventKind::RoutePlanApplied));
        QVERIFY(!hasEvent(events, NavigationEventKind::NavigationAborted));
        QVERIFY(!hasEvent(events, NavigationEventKind::RoutePlanRejected));
    }

    // 5. 不可达终点：Unreachable → NoRouteAvailable → 导航中止（Idle）。
    void unreachableDestinationFails() {
        Rig rig;
        int errorCount = 0;
        NavigationErrorCode lastError{};
        QObject::connect(rig.controller.get(),
                         &p1::contracts::NavigationController::navigationErrorOccurred,
                         [&](const p1::contracts::NavigationError& error) {
                             ++errorCount;
                             lastError = error.code;
                         });
        rig.controller->start(makeRequest(1, node900Coord, nodeCoordinate(99.0, 99.0)));
        QVERIFY(waitFor([&] { return rig.controller->status() == NavigationStatus::Idle; }));
        QVERIFY(hasEvent(rig.controller->events(), NavigationEventKind::NavigationAborted));
        QCOMPARE(errorCount, 1);
        QCOMPARE(lastError, NavigationErrorCode::NoRouteAvailable);
    }

    // 6. 端点不落在任何图节点（3 号 locateNode 精确匹配失败）→ PlanningFailed → 中止。
    void nonNodeEndpointFails() {
        Rig rig;
        int errorCount = 0;
        NavigationErrorCode lastError{};
        QObject::connect(rig.controller.get(),
                         &p1::contracts::NavigationController::navigationErrorOccurred,
                         [&](const p1::contracts::NavigationError& error) {
                             ++errorCount;
                             lastError = error.code;
                         });
        rig.controller->start(makeRequest(1, node900Coord, nodeCoordinate(3.0, 2.5)));
        QVERIFY(waitFor([&] { return rig.controller->status() == NavigationStatus::Idle; }));
        QVERIFY(hasEvent(rig.controller->events(), NavigationEventKind::NavigationAborted));
        QCOMPARE(errorCount, 1);
        QCOMPARE(lastError, NavigationErrorCode::PlanningFailed);
    }

    // 7. 桥析构静默：销毁桥（连带回收规划器与工作线程）后不再有任何事件。
    void destructorIsSilent() {
        Rig rig;
        const auto node5 = nodeCoordinate(6.0, 0.0);
        rig.controller->start(makeRequest(1, node900Coord, node5));
        rig.bridge.reset(); // 立即销毁桥：先退工作线程、再回收规划器（契约文件头 ②）
        const int eventsAtDestroy = static_cast<int>(rig.controller->events().size());
        QTest::qWait(300);
        QCOMPARE(static_cast<int>(rig.controller->events().size()), eventsAtDestroy);
        QVERIFY(!hasEvent(rig.controller->events(), NavigationEventKind::RoutePlanApplied));
        QVERIFY(!hasEvent(rig.controller->events(), NavigationEventKind::NavigationAborted));
    }

private:
    static bool hasEvent(const std::vector<p1::contracts::NavigationEvent>& events,
                         NavigationEventKind kind) {
        return std::any_of(events.begin(), events.end(),
                           [kind](const p1::contracts::NavigationEvent& e) { return e.kind == kind; });
    }
};

QTEST_MAIN(RealPlannerBridgeTests)
#include "RealPlannerBridgeTests.moc"
