// 4 号第 2 次作业：规划对接链路测试（AsyncPlannerBridge + StubRoutePlanner + RecordingController）。
// 覆盖第二次作业"对接真实计算和导航事件 / 不阻塞 UI"的对接层验收：
//   1. 端到端：start → 后台线程规划 → 队列回流 → 控制器采纳（Planning→Navigating）
//   2. 回流线程 = 控制器线程（Qt::QueuedConnection 生效证明）
//   3. 规划期间主线程事件循环不被阻塞（心跳计数）
//   4. stop() 取消在途请求，之后无任何回调（契约文件头 ③）
//   5. 迟到的陈旧结果被控制器拒绝（RoutePlanRejected，"stale requestId"，契约文件头 ④）
//   6. 桥析构后不再产生回调（契约文件头 ②）
//   7. stub 输出确定性（同一请求 → 相同 geometryRef/距离）
#include "../AsyncPlannerBridge.h"
#include "../RecordingController.h"
#include "../StubRoutePlanner.h"

#include <QtTest/QtTest>

#include <QThread>
#include <QTimer>

using namespace p1::navigation;
using p1::contracts::NavigationEventKind;
using p1::contracts::NavigationStatus;

namespace {

p1::contracts::RoutePlanRequest makeRequest(double latTo, int delayMsTag = 0) {
    p1::contracts::RoutePlanRequest request;
    request.issuedAtMs = QDateTime::currentMSecsSinceEpoch() + delayMsTag;
    request.origin.crs = p1::contracts::CoordinateSystem::WGS84;
    request.origin.latitudeDeg = 60.000000;
    request.origin.longitudeDeg = 116.000000;
    request.destination.crs = p1::contracts::CoordinateSystem::WGS84;
    request.destination.latitudeDeg = latTo;
    request.destination.longitudeDeg = 116.000000;
    request.dataVersion = QStringLiteral("assignment2-test");
    return request;
}

int countEvents(const RecordingController& controller, NavigationEventKind kind) {
    int n = 0;
    for (const auto& event : controller.events()) {
        if (event.kind == kind) {
            ++n;
        }
    }
    return n;
}

bool hasEvent(const RecordingController& controller, NavigationEventKind kind, const QString& reasonContains) {
    for (const auto& event : controller.events()) {
        if (event.kind == kind && event.reason.contains(reasonContains)) {
            return true;
        }
    }
    return false;
}

} // namespace

class PlannerBridgeTests : public QObject {
    Q_OBJECT

private slots:
    // 覆盖：端到端 + 回流线程归属（对接层核心验收）。
    void planCompletesOnControllerThread() {
        RecordingController controller;
        AsyncPlannerBridge bridge(&controller);
        auto* planner = new StubRoutePlanner();
        planner->setComputeDelayMs(50);
        bridge.attachPlanner(planner);

        // 回流线程检查：结果处理必须发生在控制器所属（当前测试）线程。
        bool resultOnMainThread = false;
        connect(&controller, &p1::contracts::NavigationController::navigationEventOccurred,
                [&](const p1::contracts::NavigationEvent& event) {
                    if (event.kind == NavigationEventKind::RoutePlanApplied) {
                        resultOnMainThread = (QThread::currentThread() == controller.thread());
                    }
                });

        const std::uint64_t id = controller.start(makeRequest(60.000900));
        QVERIFY(id != 0);
        QCOMPARE(controller.status(), NavigationStatus::Planning);

        QTRY_COMPARE_WITH_TIMEOUT(controller.status(), NavigationStatus::Navigating, 5000);
        QCOMPARE(appliedCount(controller), 1);
        QVERIFY(resultOnMainThread);
        QVERIFY(controller.lastResultOnControllerThread());

        // 采纳后的快照字段：路线、requestId、navigationId 贯通。
        const auto snapshot = controller.snapshot();
        QVERIFY(snapshot.route.has_value());
        QCOMPARE(snapshot.route->requestId, id);
        QCOMPARE(snapshot.currentRequestId, id); // 规划完成后仍在（无新请求）
        QVERIFY(snapshot.route->totalDistanceM > 0.0);
        QVERIFY(snapshot.route->totalDurationSec > 0.0);
        QCOMPARE(snapshot.route->source, p1::contracts::SourceKind::Mock);
    }

    // 覆盖：规划期间主线程事件循环照常运转（"不阻塞 UI"最直接证据）。
    void mainLoopResponsiveWhilePlanning() {
        RecordingController controller;
        AsyncPlannerBridge bridge(&controller);
        auto* planner = new StubRoutePlanner();
        planner->setComputeDelayMs(400);
        bridge.attachPlanner(planner);

        controller.start(makeRequest(60.000900));

        int heartbeats = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, [&heartbeats] { ++heartbeats; });
        heartbeat.start(40);

        QTRY_COMPARE_WITH_TIMEOUT(controller.status(), NavigationStatus::Navigating, 5000);
        heartbeat.stop();
        // 400ms / 40ms ≈ 10 次；给调度余量取 >=3，足以证明主线程未被阻塞。
        QVERIFY(heartbeats >= 3);
    }

    // 覆盖：stop() 取消在途请求，之后不再有任何事件回流（契约 ③）。
    void stopCancelsInFlightAndSilences() {
        RecordingController controller;
        AsyncPlannerBridge bridge(&controller);
        auto* planner = new StubRoutePlanner();
        planner->setComputeDelayMs(500);
        bridge.attachPlanner(planner);

        const std::uint64_t id = controller.start(makeRequest(60.000900));
        QVERIFY(id != 0);
        QTRY_COMPARE_WITH_TIMEOUT(bridge.inFlightCount(), std::uint64_t{1}, 2000);

        controller.stop();
        QCOMPARE(controller.status(), NavigationStatus::Idle);
        const int eventsAtStop = static_cast<int>(controller.events().size());

        // 若未取消，500ms 后本应回流结果；验证没有任何事件到达控制器。
        QTest::qWait(900);
        QCOMPARE(static_cast<int>(controller.events().size()), eventsAtStop);
        QCOMPARE(countEvents(controller, NavigationEventKind::RoutePlanApplied), 0);
        QCOMPARE(countEvents(controller, NavigationEventKind::NavigationStopped), 1);
    }

    // 覆盖：重规划后，被取消请求的迟到结果被拒绝（契约 ④ 陈旧 requestId）。
    // setHonorCancel(false) 模拟"取消不及时"的规划器，迟到结果必须原样回流并遭控制器拒绝。
    void staleResultRejected() {
        RecordingController controller;
        AsyncPlannerBridge bridge(&controller);
        auto* planner = new StubRoutePlanner();
        planner->setHonorCancel(false); // 故障注入：cancel 不拦截，制造迟到结果
        planner->setComputeDelayMs(300);
        bridge.attachPlanner(planner);

        // start(r1) 立即被 start(r2) 取代（转换表：Planning 中 start → 取消旧请求重新规划）。
        controller.start(makeRequest(60.000900, 1));
        const std::uint64_t id2 = controller.start(makeRequest(60.001800, 2));
        QVERIFY(id2 != 0);

        // r2 先正常完成 → Navigating；r1 的迟到结果随后到达，必须被拒绝且不影响状态。
        QTRY_COMPARE_WITH_TIMEOUT(controller.status(), NavigationStatus::Navigating, 5000);
        QTest::qWait(400); // r1 剩余延迟
        QCOMPARE(controller.status(), NavigationStatus::Navigating);
        QVERIFY(hasEvent(controller, NavigationEventKind::RoutePlanRejected, QStringLiteral("stale requestId")));
        QCOMPARE(countEvents(controller, NavigationEventKind::RoutePlanApplied), 1); // 只有 r2 被采纳
        QCOMPARE(controller.snapshot().route->requestId, id2);
    }

    // 覆盖：桥析构后不再产生任何回调（契约 ②）。
    void destructorSilencesCallbacks() {
        RecordingController controller;
        auto* planner = new StubRoutePlanner();
        planner->setComputeDelayMs(500);
        {
            AsyncPlannerBridge bridge(&controller);
            bridge.attachPlanner(planner); // 所有权移交桥
            controller.start(makeRequest(60.000900));
            QVERIFY(bridge.inFlightCount() >= 1);
        } // 桥在此析构：工作线程退出，在途请求作废
        const int eventsAtDestroy = static_cast<int>(controller.events().size());
        QTest::qWait(900); // 若未正确清理，500ms 规划已完成并回流
        QCOMPARE(static_cast<int>(controller.events().size()), eventsAtDestroy);
    }

    // 覆盖：stub 输出确定性（同一请求两次规划结果除 requestId 外完全一致）。
    void stubOutputIsDeterministic() {
        RecordingController controller;
        AsyncPlannerBridge bridge(&controller);
        auto* planner = new StubRoutePlanner();
        planner->setComputeDelayMs(1);
        bridge.attachPlanner(planner);

        controller.start(makeRequest(60.000900));
        QTRY_COMPARE_WITH_TIMEOUT(controller.status(), NavigationStatus::Navigating, 5000);
        const auto first = controller.snapshot().route;
        controller.stop();

        controller.start(makeRequest(60.000900));
        QTRY_COMPARE_WITH_TIMEOUT(controller.status(), NavigationStatus::Navigating, 5000);
        const auto second = controller.snapshot().route;
        controller.stop();

        QVERIFY(first.has_value() && second.has_value());
        QCOMPARE(first->geometryRef, second->geometryRef);
        QCOMPARE(first->totalDistanceM, second->totalDistanceM);
        QCOMPARE(first->totalDurationSec, second->totalDurationSec);
        QCOMPARE(first->routeId, QStringLiteral("stub-route-%1").arg(first->requestId)); // routeId 可由 requestId 复算
        QVERIFY(first->requestId != second->requestId);                                  // requestId 单调不重复
    }

private:
    static int appliedCount(const RecordingController& controller) {
        return countEvents(controller, NavigationEventKind::RoutePlanApplied);
    }
};

QTEST_MAIN(PlannerBridgeTests)
#include "PlannerBridgeTests.moc"
