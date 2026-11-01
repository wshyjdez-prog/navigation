// 4 号第 2 次作业演示：规划对接链路（"对接真实计算和导航事件"）。
// 链路：RecordingController(契约) → AsyncPlannerBridge(后台线程) → StubRoutePlanner(确定性桩)
//       → routeReady 经队列连接回流 → 控制器采纳 → 导航事件输出。
// 本程序退出码 0 即演示成功；注册为 ctest navigation-planner-demo。
//
// 用法：p1-planner-demo [延迟毫秒]（默认 300，用于观察主线程心跳不被阻塞）
#include "../AsyncPlannerBridge.h"
#include "../RecordingController.h"
#include "../StubRoutePlanner.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>

using namespace p1::navigation;
using p1::contracts::NavigationEventKind;

namespace {

void printEvent(const p1::contracts::NavigationEvent& event) {
    std::printf("  [event] %-20s status=%-10s req=%llu nav=%llu reason=%s\n",
                p1::contracts::toString(event.kind), p1::contracts::toString(event.status),
                static_cast<unsigned long long>(event.requestId),
                static_cast<unsigned long long>(event.navigationId), qPrintable(event.reason));
}

p1::contracts::RoutePlanRequest makeRequest(double latFrom, double lonFrom, double latTo, double lonTo) {
    p1::contracts::RoutePlanRequest request;
    request.issuedAtMs = QDateTime::currentMSecsSinceEpoch();
    request.origin.crs = p1::contracts::CoordinateSystem::WGS84;
    request.origin.latitudeDeg = latFrom;
    request.origin.longitudeDeg = lonFrom;
    request.destination.crs = p1::contracts::CoordinateSystem::WGS84;
    request.destination.latitudeDeg = latTo;
    request.destination.longitudeDeg = lonTo;
    request.dataVersion = QStringLiteral("assignment2-demo");
    return request;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    int delayMs = 300;
    if (argc > 1) {
        bool ok = false;
        const int parsed = QString::fromLocal8Bit(argv[1]).toInt(&ok);
        if (ok && parsed >= 0 && parsed <= 10000) {
            delayMs = parsed;
        }
    }

    std::printf("=== P1 planner bridge demo (stub planner, %d ms simulated compute) ===\n", delayMs);

    RecordingController controller;
    AsyncPlannerBridge bridge(&controller);
    auto* planner = new StubRoutePlanner(); // 所有权移交桥（析构统一回收）
    planner->setComputeDelayMs(delayMs);
    bridge.attachPlanner(planner);

    // 导航事件对接：控制器事件 → 演示输出（正式工程中即 1 号页面 / 5 号渲染 / 日志）。
    int appliedCount = 0;
    QObject::connect(&controller, &p1::contracts::NavigationController::navigationEventOccurred,
                     [&](const p1::contracts::NavigationEvent& event) {
                         printEvent(event);
                         if (event.kind == NavigationEventKind::RoutePlanApplied) {
                             ++appliedCount;
                         }
                     });

    // 主线程心跳：规划进行期间事件循环必须照常运转（不阻塞 UI 的最直接证明）。
    int heartbeats = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&heartbeats] {
        ++heartbeats;
        std::printf("  [main-thread heartbeat %d] UI loop alive during planning\n", heartbeats);
    });

    // ── 阶段 1：完整规划链路 ─────────────────────────────────────────────
    std::printf("--- phase 1: start -> async plan -> route applied ---\n");
    heartbeat.start(60);
    const std::uint64_t navRequestId = controller.start(makeRequest(60.000000, 116.000000, 60.000900, 116.000000));
    if (navRequestId == 0) {
        std::printf("FAIL: start rejected\n");
        return 1;
    }
    std::printf("  start() accepted, requestId=%llu, waiting on background thread...\n",
                static_cast<unsigned long long>(navRequestId));

    QEventLoop waitApplied;
    QTimer::singleShot(10 * 1000, &waitApplied, &QEventLoop::quit); // 超时保护
    QObject::connect(&controller, &p1::contracts::NavigationController::navigationEventOccurred,
                     &waitApplied, [&](const p1::contracts::NavigationEvent& event) {
                         if (event.kind == NavigationEventKind::RoutePlanApplied ||
                             event.kind == NavigationEventKind::NavigationAborted) {
                             waitApplied.quit();
                         }
                     });
    waitApplied.exec();
    heartbeat.stop();

    if (controller.status() != p1::contracts::NavigationStatus::Navigating || appliedCount < 1) {
        std::printf("FAIL: plan did not complete (status=%s)\n", p1::contracts::toString(controller.status()));
        return 1;
    }
    const auto snapshot = controller.snapshot();
    std::printf("  applied: route=%s distance=%.2f m duration=%.2f s\n", qPrintable(snapshot.route->routeId),
                snapshot.route->totalDistanceM, snapshot.route->totalDurationSec);
    std::printf("  heartbeats during planning: %d (>=3 expected => UI loop not blocked)\n", heartbeats);
    if (heartbeats < 3) {
        std::printf("FAIL: main thread appears blocked during planning\n");
        return 1;
    }

    // ── 阶段 2：stop() 取消在途请求 → 之后不得再回流任何结果 ────────────
    std::printf("--- phase 2: stop() cancels in-flight request, no callbacks afterwards ---\n");
    controller.stop(); // 回 Idle（发出 NavigationStopped + routePlanCancelled）

    controller.start(makeRequest(60.000000, 116.000000, 60.001800, 116.000000)); // 新在途请求
    controller.stop(); // 立即取消
    const int eventsAtSecondStop = static_cast<int>(controller.events().size());

    // 给足"若未取消本应完成"的时间，验证没有迟到的结果回流。
    QEventLoop waitSilence;
    QTimer::singleShot(delayMs + 500, &waitSilence, &QEventLoop::quit);
    waitSilence.exec();

    const int eventsAfterSilence = static_cast<int>(controller.events().size());
    if (eventsAfterSilence != eventsAtSecondStop) {
        std::printf("FAIL: %d unexpected event(s) after stop()\n", eventsAfterSilence - eventsAtSecondStop);
        return 1;
    }
    std::printf("  no callbacks after stop(): %d events before == %d after (PASS)\n", eventsAtSecondStop,
                eventsAfterSilence);

    std::printf("=== demo OK ===\n");
    return 0;
}
