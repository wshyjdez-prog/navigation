// 4 号导航状态事件契约测试（Stage 5）。
// 被测对象：include/contracts/NavigationTypes.h、include/contracts/NavigationController.h。
// 全部数据为 Mock，不接真实定位、不接 3 号算法、不接 G6。
//
// 说明：契约里的 NavigationController 是抽象接口，状态机实现属第 3 次作业（本次不实现）。
// 为了对"转换表逐条断言""非法事件按约定处理""旧 requestId 结果被丢弃"做可执行验证，
// 本文件自带一个 ReferenceController：它只是把契约第 ⑤ 节的转换表和第 ⑥ 节的偏航/重算策略
// 机械地翻译成代码，**不含任何几何、匹配、规划算法**（剩余距离由外部注入的 provider 提供），
// 它是测试脚手架，不是第 3 次作业的状态机实现。
#include "contracts/NavigationController.h"

#include <QDateTime>
#include <QtTest>

#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

using namespace p1::contracts;

namespace contractref {

struct RemainingEstimate {
    double distanceM = 0.0;
    double durationSec = 0.0;
};
// 剩余距离/时长的注入点：几何计算属第 3 次作业，这里只留接口。
using RemainingProvider =
    std::function<std::optional<RemainingEstimate>(const PositionSample&, const RoutePlanRef&)>;

class ReferenceController final : public NavigationController {
    Q_OBJECT
public:
    explicit ReferenceController(QObject* parent = nullptr) : NavigationController(parent) {}
    void setRemainingProvider(RemainingProvider provider) { provider_ = std::move(provider); }

    NavigationSnapshot snapshot() const override {
        NavigationSnapshot s;
        s.status = status_;
        s.paused = paused_;
        s.navigationId = navigationId_;
        s.currentRequestId = currentRequestId_;
        s.route = route_;
        s.currentPosition = position_;
        s.remainingDistanceM = remainingM_;
        s.remainingDurationSec = remainingS_;
        s.guidance = guidance_;
        s.guidanceIndex = guidanceIndex_;
        s.dataVersion = dataVersion_;
        s.source = SourceKind::Mock;
        s.updatedAtMs = QDateTime::currentMSecsSinceEpoch();
        return s;
    }
    NavigationStatus status() const override { return status_; }
    bool paused() const override { return paused_; }
    std::uint64_t navigationId() const override { return navigationId_; }
    std::uint64_t currentRequestId() const override { return currentRequestId_; }

    std::uint64_t start(const RoutePlanRequest& request) override {
        if (status_ != NavigationStatus::Idle) {           // 转换表：非 Idle 的 start 一律丢弃
            emitEvent(NavigationEventKind::IllegalEventDropped,
                      QStringLiteral("start 只能在 Idle 调用，当前状态 %1").arg(toString(status_)));
            return 0;
        }
        silent_ = false;
        ++navigationId_;
        const std::uint64_t id = request.requestId > 0 ? request.requestId : ++nextRequestId_;
        currentRequestId_ = id;
        destination_ = request.destination;
        dataVersion_ = request.dataVersion;
        status_ = NavigationStatus::Planning;
        emitEvent(NavigationEventKind::NavigationStarted, QStringLiteral("开始导航"), std::nullopt, id);
        emitEvent(NavigationEventKind::RoutePlanningStarted, QStringLiteral("发起路线规划"), std::nullopt, id);
        emitStatus();
        emit routePlanRequested(request);
        return id;
    }

    bool pause() override {
        if (silent_) return false;
        if (status_ != NavigationStatus::Navigating && status_ != NavigationStatus::OffRoute) {
            emitEvent(NavigationEventKind::IllegalEventDropped,
                      QStringLiteral("pause 只在 Navigating/OffRoute 有效，当前状态 %1").arg(toString(status_)));
            return false;
        }
        if (paused_) return false;
        paused_ = true;
        emitEvent(NavigationEventKind::NavigationPaused, QStringLiteral("暂停导航"));
        emitStatus();
        return true;
    }

    bool resume() override {
        if (silent_) return false;
        if (!paused_) {
            emitEvent(NavigationEventKind::IllegalEventDropped, QStringLiteral("resume 但当前未暂停"));
            return false;
        }
        paused_ = false;
        emitEvent(NavigationEventKind::NavigationResumed, QStringLiteral("恢复导航"));
        emitStatus();
        return true;
    }

    void stop() override {
        if (status_ == NavigationStatus::Idle) return; // 空操作（含重复 stop）
        if (currentRequestId_ != 0) {
            emit routePlanCancelled(currentRequestId_);
            currentRequestId_ = 0;
        }
        resetNavigationState();
        silent_ = true;                                  // ③ stop() 后进入静默期
        emitEvent(NavigationEventKind::NavigationStopped, QStringLiteral("stop() 停止导航"));
        emitStatus();
    }

    void locationUpdated(const PositionSample& sample) override {
        if (silent_) return;                             // 静默期：静默丢弃
        if (status_ == NavigationStatus::Idle) return;   // 未导航：定位源可与导航解耦，不计错误事件
        position_ = sample;
        if (status_ == NavigationStatus::Planning) return; // 只记录，不改状态
        if (paused_) return;                             // 暂停：只更新位置，不推进引导与剩余量
        if (provider_ && route_) {
            if (const auto estimate = provider_(sample, *route_)) {
                remainingM_ = estimate->distanceM;
                remainingS_ = estimate->durationSec;
                emit snapshotUpdated(snapshot());
            }
        }
    }

    std::uint64_t requestReroute(const RerouteRequest& request) override {
        if (silent_) return 0;
        if (status_ != NavigationStatus::Navigating && status_ != NavigationStatus::OffRoute) {
            emitEvent(NavigationEventKind::IllegalEventDropped,
                      QStringLiteral("requestReroute 需要有效路线，当前状态 %1").arg(toString(status_)));
            return 0;
        }
        const std::uint64_t id = request.requestId > 0 ? request.requestId : ++nextRequestId_;
        currentRequestId_ = id;
        lastRerouteAtMs_ = request.issuedAtMs;
        destination_ = request.destination;
        emitEvent(NavigationEventKind::RerouteRequested, QStringLiteral("外部请求重算"),
                  request.locationSeq, id);
        RoutePlanRequest plan;
        plan.requestId = id;
        plan.issuedAtMs = request.issuedAtMs;
        plan.origin = request.currentPosition;
        plan.destination = request.destination;
        plan.dataVersion = dataVersion_;
        emit routePlanRequested(plan);
        return id;
    }

    void routeResultReady(const RoutePlanResult& result) override {
        if (silent_) return;
        const bool stale = result.requestId == 0 || result.requestId != currentRequestId_;
        if (stale) {                                     // ④ 旧任务异步结果不得覆盖当前导航
            emitEvent(status_ == NavigationStatus::OffRoute ? NavigationEventKind::RerouteRejected
                                                            : NavigationEventKind::RoutePlanRejected,
                      QStringLiteral("stale requestId %1，当前有效请求为 %2")
                          .arg(result.requestId)
                          .arg(currentRequestId_),
                      std::nullopt, result.requestId);
            return;
        }
        currentRequestId_ = 0;
        if (!result.route) {                             // 结果形状不完整，按失败处理
            if (status_ == NavigationStatus::Planning) {
                resetNavigationState();
                silent_ = true;
                emitError(NavigationErrorCode::NoRouteAvailable, QStringLiteral("规划结果缺少路线"), result.requestId);
                emitEvent(NavigationEventKind::NavigationAborted, QStringLiteral("无可用路线，导航中止"),
                          std::nullopt, result.requestId);
                emitStatus();
            } else {
                emitError(NavigationErrorCode::PlanningFailed, QStringLiteral("重算结果缺少路线"), result.requestId);
            }
            return;
        }
        route_ = *result.route;
        instructions_ = result.instructions;
        remainingM_ = route_->totalDistanceM;
        remainingS_ = route_->totalDurationSec;
        const bool wasPlanning = status_ == NavigationStatus::Planning;
        status_ = NavigationStatus::Navigating;
        emitEvent(wasPlanning ? NavigationEventKind::RoutePlanApplied : NavigationEventKind::RouteUpdated,
                  wasPlanning ? QStringLiteral("首个路线已应用") : QStringLiteral("重算路线已应用"),
                  std::nullopt, result.requestId);
        emitStatus();
        if (!instructions_.empty()) {
            guidance_ = instructions_.front();
            guidanceIndex_ = guidance_->index;
            emit guidanceUpdated(*guidance_);
        }
        emit snapshotUpdated(snapshot());
    }

    void routeFailed(std::uint64_t requestId, NavigationErrorCode code, const QString& message) override {
        if (silent_) return;
        if (requestId == 0 || requestId != currentRequestId_) {
            emitEvent(status_ == NavigationStatus::OffRoute ? NavigationEventKind::RerouteRejected
                                                            : NavigationEventKind::RoutePlanRejected,
                      QStringLiteral("stale requestId %1，当前有效请求为 %2").arg(requestId).arg(currentRequestId_),
                      std::nullopt, requestId);
            return;
        }
        currentRequestId_ = 0;
        if (status_ == NavigationStatus::Planning) {
            const bool cancelled = code == NavigationErrorCode::PlanningCancelled;
            resetNavigationState();
            silent_ = true;
            if (!cancelled) {
                emitError(code, message, requestId);
                emitEvent(NavigationEventKind::NavigationAborted, QStringLiteral("规划失败，导航中止"),
                          std::nullopt, requestId);
            } else {
                emitEvent(NavigationEventKind::NavigationStopped, QStringLiteral("规划被取消"),
                          std::nullopt, requestId);
            }
            emitStatus();
            return;
        }
        // 重算失败：保持当前状态，继续用旧路线导航
        emitError(code, message, requestId);
    }

    void arrived(std::uint64_t locationSeq) override {
        if (silent_) return;
        if (status_ != NavigationStatus::Navigating && status_ != NavigationStatus::OffRoute) {
            emitEvent(NavigationEventKind::IllegalEventDropped,
                      QStringLiteral("arrived 只在 Navigating/OffRoute 有效，当前状态 %1").arg(toString(status_)),
                      locationSeq);
            return;
        }
        status_ = NavigationStatus::Arrived;
        paused_ = false;
        currentRequestId_ = 0;
        remainingM_ = 0.0;
        remainingS_ = 0.0;
        for (const auto& instruction : instructions_) {
            if (instruction.kind == GuidanceKind::ArriveDestination) {
                guidance_ = instruction;
                guidanceIndex_ = instruction.index;
                emit guidanceUpdated(instruction);
            }
        }
        emitEvent(NavigationEventKind::Arrived, QStringLiteral("到达终点"), locationSeq);
        emitStatus();
    }

    void offRoute(std::uint64_t locationSeq) override {
        if (silent_) return;
        if (status_ == NavigationStatus::OffRoute) {     // 重复偏航：去重，不重复触发重算
            emitEvent(NavigationEventKind::IllegalEventDropped, QStringLiteral("already off-route"), locationSeq);
            return;
        }
        if (status_ != NavigationStatus::Navigating) {
            emitEvent(NavigationEventKind::IllegalEventDropped,
                      QStringLiteral("offRoute 只在 Navigating 有效，当前状态 %1").arg(toString(status_)),
                      locationSeq);
            return;
        }
        status_ = NavigationStatus::OffRoute;
        emitEvent(NavigationEventKind::OffRouteDetected, QStringLiteral("偏离路线"), locationSeq);
        emitStatus();
        maybeRequestReroute(locationSeq);
    }

private:
    void maybeRequestReroute(std::uint64_t locationSeq) {
        if (currentRequestId_ != 0) return;              // ⑥ 重算在途：不发起新请求
        const std::int64_t now = position_ ? position_->timestampMs : 0;
        if (lastRerouteAtMs_ > 0 && now - lastRerouteAtMs_ < kMinRerouteIntervalMs) return; // ⑥ 最小间隔 10 秒
        const std::uint64_t id = ++nextRequestId_;
        currentRequestId_ = id;
        lastRerouteAtMs_ = now;
        emitEvent(NavigationEventKind::RerouteRequested, QStringLiteral("偏航后发起重算"), locationSeq, id);
        RoutePlanRequest plan;
        plan.requestId = id;
        plan.issuedAtMs = now;
        plan.origin = position_ ? position_->coordinate : GeoCoordinate{};
        plan.destination = destination_;
        plan.dataVersion = dataVersion_;
        emit routePlanRequested(plan);
    }

    void resetNavigationState() {
        status_ = NavigationStatus::Idle;
        paused_ = false;
        currentRequestId_ = 0;
        route_.reset();
        instructions_.clear();
        guidance_.reset();
        guidanceIndex_.reset();
        remainingM_.reset();
        remainingS_.reset();
        position_.reset();
        lastRerouteAtMs_ = 0;
    }

    void emitStatus() { emit statusChanged(status_, paused_); }

    void emitEvent(NavigationEventKind kind, const QString& reason,
                   std::optional<std::uint64_t> locationSeq = std::nullopt, std::uint64_t requestId = 0) {
        NavigationEvent event;
        event.kind = kind;
        event.status = status_;
        event.paused = paused_;
        event.timestampMs = QDateTime::currentMSecsSinceEpoch();
        event.locationSeq = locationSeq;
        event.requestId = requestId;
        event.navigationId = navigationId_;
        event.reason = reason;
        event.source = SourceKind::Mock;
        emit navigationEventOccurred(event);
    }

    void emitError(NavigationErrorCode code, const QString& message, std::uint64_t requestId) {
        NavigationError error;
        error.code = code;
        error.message = message;
        error.timestampMs = QDateTime::currentMSecsSinceEpoch();
        error.requestId = requestId;
        error.navigationId = navigationId_;
        error.source = SourceKind::Mock;
        emit navigationErrorOccurred(error);
    }

    static constexpr std::int64_t kMinRerouteIntervalMs = 10000; // ⑥ 重算最小间隔 10 秒

    NavigationStatus status_ = NavigationStatus::Idle;
    bool paused_ = false;
    bool silent_ = false;
    std::uint64_t navigationId_ = 0;
    std::uint64_t nextRequestId_ = 0;
    std::uint64_t currentRequestId_ = 0;
    std::int64_t lastRerouteAtMs_ = 0;
    std::optional<RoutePlanRef> route_;
    std::optional<PositionSample> position_;
    std::optional<double> remainingM_;
    std::optional<double> remainingS_;
    std::optional<GuidanceInstruction> guidance_;
    std::optional<std::uint32_t> guidanceIndex_;
    std::vector<GuidanceInstruction> instructions_;
    GeoCoordinate destination_;
    QString dataVersion_;
    RemainingProvider provider_;
};

} // namespace contractref

namespace {

using contractref::ReferenceController;
using contractref::RemainingEstimate;
using contractref::RemainingProvider;

// 记录控制器发出的全部信号，供转换表逐条断言。
struct ControllerLog {
    std::vector<NavigationEvent> events;
    std::vector<NavigationError> errors;
    std::vector<NavigationStatus> statuses;
    std::vector<GuidanceInstruction> guidance;
    std::vector<RoutePlanRequest> routeRequests;
    std::vector<std::uint64_t> cancellations;
    int snapshots = 0;

    void attach(ReferenceController& controller) {
        QObject::connect(&controller, &NavigationController::navigationEventOccurred, &controller,
                         [this](const NavigationEvent& e) { events.push_back(e); });
        QObject::connect(&controller, &NavigationController::navigationErrorOccurred, &controller,
                         [this](const NavigationError& e) { errors.push_back(e); });
        QObject::connect(&controller, &NavigationController::statusChanged, &controller,
                         [this](NavigationStatus s, bool) { statuses.push_back(s); });
        QObject::connect(&controller, &NavigationController::guidanceUpdated, &controller,
                         [this](const GuidanceInstruction& g) { guidance.push_back(g); });
        QObject::connect(&controller, &NavigationController::routePlanRequested, &controller,
                         [this](const RoutePlanRequest& r) { routeRequests.push_back(r); });
        QObject::connect(&controller, &NavigationController::routePlanCancelled, &controller,
                         [this](std::uint64_t id) { cancellations.push_back(id); });
        QObject::connect(&controller, &NavigationController::snapshotUpdated, &controller,
                         [this](const NavigationSnapshot&) { ++snapshots; });
    }
    int count(NavigationEventKind kind) const {
        int n = 0;
        for (const auto& e : events)
            if (e.kind == kind) ++n;
        return n;
    }
    bool has(NavigationEventKind kind) const { return count(kind) > 0; }
    std::size_t total() const {
        return events.size() + errors.size() + statuses.size() + guidance.size() + routeRequests.size()
            + cancellations.size() + static_cast<std::size_t>(snapshots);
    }
};

RoutePlanRef makeRoute(std::uint64_t requestId, double totalDistanceM = 190.3572) {
    RoutePlanRef route;
    route.routeId = QStringLiteral("stub-route-straight-north-001");
    route.dataVersion = QStringLiteral("fixture-navigation-case-v1");
    route.geometryRef = QStringLiteral("stub://waypoints/straight-north-001");
    route.totalDistanceM = totalDistanceM;
    route.totalDurationSec = 19.0;
    route.source = SourceKind::Mock;
    route.requestId = requestId;
    return route;
}

RoutePlanResult makeResult(std::uint64_t requestId, bool withRoute, double totalDistanceM = 190.3572) {
    RoutePlanResult result;
    result.requestId = requestId;
    if (withRoute) {
        result.route = makeRoute(requestId, totalDistanceM);
        GuidanceInstruction depart;
        depart.kind = GuidanceKind::Depart;
        depart.text = QStringLiteral("出发，沿虚拟道路向北");
        depart.distanceToManeuverM = 0.0;
        depart.index = 0;
        GuidanceInstruction arrive;
        arrive.kind = GuidanceKind::ArriveDestination;
        arrive.text = QStringLiteral("到达目的地");
        arrive.distanceToManeuverM = totalDistanceM;
        arrive.index = 1;
        result.instructions = {depart, arrive};
    }
    return result;
}

RoutePlanRequest makeRequest(std::uint64_t requestId) {
    RoutePlanRequest request;
    request.requestId = requestId;
    request.issuedAtMs = 1767225600000;
    request.origin = GeoCoordinate{CoordinateSystem::WGS84, 60.000000, 116.000000, std::nullopt};
    request.destination = GeoCoordinate{CoordinateSystem::WGS84, 60.001710, 116.000000, std::nullopt};
    request.dataVersion = QStringLiteral("fixture-navigation-case-v1");
    return request;
}

PositionSample makeSample(std::int64_t timestampMs, double latDeg, std::uint64_t seq = 0) {
    PositionSample sample;
    sample.coordinate = GeoCoordinate{CoordinateSystem::WGS84, latDeg, 116.000000, 50.0};
    sample.timestampMs = timestampMs;
    sample.speedMps = 10.02;
    sample.bearingDeg = 0.0;
    sample.horizontalAccuracyM = 5.0;
    sample.source = SourceKind::Mock;
    sample.seq = seq;
    sample.sessionId = 1;
    sample.dataVersion = QStringLiteral("fixture-normal-straight-v1");
    return sample;
}

// 线性剩余量提供器：进度按纬度差 × 111320 米/度，时长 = 距离 ÷ 10.0188 米/秒。
RemainingProvider linearProvider(double totalDistanceM = 190.3572) {
    return [totalDistanceM](const PositionSample& sample, const RoutePlanRef&) -> std::optional<RemainingEstimate> {
        const double progressed = (sample.coordinate.latitudeDeg - 60.0) * 111320.0;
        RemainingEstimate estimate;
        estimate.distanceM = totalDistanceM - progressed;
        estimate.durationSec = estimate.distanceM / 10.0188;
        return estimate;
    };
}

// 让控制器进入 Navigating（start + 首个规划结果），并清空日志便于后续断言。
std::uint64_t startNavigating(ReferenceController& controller, ControllerLog& log) {
    const std::uint64_t requestId = controller.start(makeRequest(1));
    controller.routeResultReady(makeResult(requestId, true));
    log.events.clear();
    log.statuses.clear();
    log.guidance.clear();
    log.errors.clear();
    log.routeRequests.clear();
    log.snapshots = 0;
    return requestId;
}

} // namespace

class NavigationContractTests : public QObject {
    Q_OBJECT
private slots:
    // 覆盖 Stage 2：NavigationStatus 五态枚举与 NavigationEvent / NavigationError / 引导指令可构造，
    // 且所有枚举值都有稳定字符串（不返回 "?"），便于日志与 G6 事件映射。
    void enumerationsAndEventsAreConstructible() {
        const NavigationStatus statuses[] = {NavigationStatus::Idle, NavigationStatus::Planning,
                                             NavigationStatus::Navigating, NavigationStatus::OffRoute,
                                             NavigationStatus::Arrived};
        for (const auto status : statuses)
            QVERIFY2(QString::fromLatin1(toString(status)) != QStringLiteral("?"), toString(status));
        const NavigationEventKind kinds[] = {
            NavigationEventKind::NavigationStarted,  NavigationEventKind::RoutePlanningStarted,
            NavigationEventKind::RoutePlanApplied,   NavigationEventKind::RoutePlanRejected,
            NavigationEventKind::NavigationPaused,   NavigationEventKind::NavigationResumed,
            NavigationEventKind::OffRouteDetected,   NavigationEventKind::RerouteRequested,
            NavigationEventKind::RouteUpdated,       NavigationEventKind::RerouteRejected,
            NavigationEventKind::GuidanceUpdated,    NavigationEventKind::Arrived,
            NavigationEventKind::NavigationStopped,  NavigationEventKind::NavigationAborted,
            NavigationEventKind::IllegalEventDropped};
        for (const auto kind : kinds) {
            NavigationEvent event;
            event.kind = kind;
            event.status = NavigationStatus::Navigating;
            event.timestampMs = 1767225600000;
            event.locationSeq = 7;
            event.requestId = 3;
            event.navigationId = 1;
            event.reason = QStringLiteral("unit");
            event.source = SourceKind::Mock;
            QVERIFY2(QString::fromLatin1(toString(kind)) != QStringLiteral("?"), toString(kind));
            QCOMPARE(event.kind, kind);
            QCOMPARE(*event.locationSeq, std::uint64_t(7));
        }
        const NavigationErrorCode codes[] = {NavigationErrorCode::NoRouteAvailable,
                                             NavigationErrorCode::PlanningFailed,
                                             NavigationErrorCode::PlanningCancelled,
                                             NavigationErrorCode::AbortedByLocationLost};
        for (const auto code : codes)
            QVERIFY2(QString::fromLatin1(toString(code)) != QStringLiteral("?"), toString(code));

        GuidanceInstruction instruction;
        instruction.kind = GuidanceKind::TurnLeft;
        instruction.text = QStringLiteral("前方左转");
        instruction.distanceToManeuverM = 50.094;
        instruction.index = 3;
        QCOMPARE(instruction.index, std::uint32_t(3));
        QVERIFY(instruction.kind == GuidanceKind::TurnLeft);
    }

    // 覆盖 Stage 2：NavigationSnapshot 字段完整性（13 个字段全部可读写，含 mock/real 标记与数据版本）。
    void snapshotCarriesEveryContractField() {
        NavigationSnapshot snapshot;
        snapshot.status = NavigationStatus::Navigating;
        snapshot.paused = true;
        snapshot.navigationId = 5;
        snapshot.currentRequestId = 9;
        snapshot.route = makeRoute(9);
        snapshot.currentPosition = makeSample(1767225605000, 60.000450, 5);
        snapshot.remainingDistanceM = 140.2632;
        snapshot.remainingDurationSec = 14.0;
        GuidanceInstruction instruction;
        instruction.kind = GuidanceKind::ArriveDestination;
        instruction.text = QStringLiteral("到达目的地");
        instruction.distanceToManeuverM = 140.2632;
        instruction.index = 1;
        snapshot.guidance = instruction;
        snapshot.guidanceIndex = instruction.index;
        snapshot.dataVersion = QStringLiteral("fixture-navigation-case-v1");
        snapshot.source = SourceKind::Mock;
        snapshot.updatedAtMs = 1767225605000;

        QVERIFY(snapshot.status == NavigationStatus::Navigating);
        QVERIFY(snapshot.paused);
        QCOMPARE(snapshot.navigationId, std::uint64_t(5));
        QCOMPARE(snapshot.currentRequestId, std::uint64_t(9));
        QVERIFY(snapshot.route.has_value());
        QCOMPARE(snapshot.route->routeId, QStringLiteral("stub-route-straight-north-001"));
        QVERIFY(snapshot.currentPosition.has_value());
        QVERIFY(snapshot.currentPosition->source == SourceKind::Mock);
        QVERIFY(std::fabs(*snapshot.remainingDistanceM - 140.2632) < 1e-6);
        QVERIFY(std::fabs(*snapshot.remainingDurationSec - 14.0) < 1e-6);
        QVERIFY(snapshot.guidance.has_value());
        QVERIFY(snapshot.guidanceIndex.has_value());
        QCOMPARE(*snapshot.guidanceIndex, std::uint32_t(1));
        QCOMPARE(snapshot.dataVersion, QStringLiteral("fixture-navigation-case-v1"));
        QVERIFY(snapshot.source == SourceKind::Mock);
        QCOMPARE(snapshot.updatedAtMs, std::int64_t(1767225605000));
    }

    // 覆盖转换表 Idle/start：Idle → Planning，生成 navigationId 与 requestId，并发出规划请求。
    void startFromIdleGoesPlanning() {
        ReferenceController controller;
        ControllerLog log;
        log.attach(controller);
        const std::uint64_t requestId = controller.start(makeRequest(1));
        QCOMPARE(requestId, std::uint64_t(1));
        QVERIFY(controller.status() == NavigationStatus::Planning);
        QCOMPARE(controller.navigationId(), std::uint64_t(1));
        QCOMPARE(controller.currentRequestId(), requestId);
        QVERIFY(log.has(NavigationEventKind::NavigationStarted));
        QVERIFY(log.has(NavigationEventKind::RoutePlanningStarted));
        QCOMPARE(log.routeRequests.size(), std::size_t(1));
        QCOMPARE(log.routeRequests[0].requestId, requestId);
        QVERIFY(log.statuses.back() == NavigationStatus::Planning);

        // 转换表 Idle/start 的第二条：非 Idle 的 start 被丢弃（返回 0，不改变状态）。
        const std::uint64_t again = controller.start(makeRequest(2));
        QCOMPARE(again, std::uint64_t(0));
        QVERIFY(controller.status() == NavigationStatus::Planning);
        QVERIFY(log.has(NavigationEventKind::IllegalEventDropped));
    }

    // 覆盖转换表 Planning/routeResultReady + ④：只有当前 requestId 的结果被采纳。
    void planningAcceptsOnlyCurrentRequestId() {
        ReferenceController controller;
        ControllerLog log;
        log.attach(controller);
        const std::uint64_t requestId = controller.start(makeRequest(1));

        controller.routeResultReady(makeResult(requestId + 99, true)); // 陈旧结果
        QVERIFY(controller.status() == NavigationStatus::Planning);
        QCOMPARE(log.count(NavigationEventKind::RoutePlanRejected), 1);
        QVERIFY(log.events.back().reason.contains(QStringLiteral("stale requestId")));

        controller.routeResultReady(makeResult(requestId, true)); // 当前结果
        QVERIFY(controller.status() == NavigationStatus::Navigating);
        QVERIFY(log.has(NavigationEventKind::RoutePlanApplied));
        QCOMPARE(log.count(NavigationEventKind::RoutePlanRejected), 1); // 未被重复丢弃
        QVERIFY(controller.snapshot().route.has_value());
        QCOMPARE(log.guidance.size(), std::size_t(1)); // 首条引导 Depart
        QCOMPARE(log.guidance[0].index, std::uint32_t(0));
    }

    // 覆盖转换表 Planning 的失败分支：无路线/计算失败 → Idle + 错误 + NavigationAborted；
    // 计算取消 → Idle + NavigationStopped 且不算故障。
    void planningFailuresAbortToIdle_data() {
        QTest::addColumn<int>("code");
        QTest::addColumn<bool>("expectError");
        QTest::addColumn<bool>("expectAborted");
        QTest::newRow("no-route") << int(NavigationErrorCode::NoRouteAvailable) << true << true;
        QTest::newRow("failed") << int(NavigationErrorCode::PlanningFailed) << true << true;
        QTest::newRow("cancelled") << int(NavigationErrorCode::PlanningCancelled) << false << false;
    }
    void planningFailuresAbortToIdle() {
        QFETCH(int, code);
        QFETCH(bool, expectError);
        QFETCH(bool, expectAborted);
        ReferenceController controller;
        ControllerLog log;
        log.attach(controller);
        const std::uint64_t requestId = controller.start(makeRequest(1));
        controller.routeFailed(requestId, NavigationErrorCode(code), QStringLiteral("stub failure"));

        QVERIFY(controller.status() == NavigationStatus::Idle);
        QCOMPARE(log.errors.size(), std::size_t(expectError ? 1 : 0));
        QCOMPARE(log.count(NavigationEventKind::NavigationAborted), expectAborted ? 1 : 0);
        QCOMPARE(log.count(NavigationEventKind::NavigationStopped), expectAborted ? 0 : 1);
        if (expectError) QVERIFY(log.errors[0].code == NavigationErrorCode(code));
        QVERIFY(log.statuses.back() == NavigationStatus::Idle);
    }

    // 覆盖转换表 Navigating/pause|resume 与 Planning 下的非法暂停。
    void pauseAndResumeOnlyWhileNavigating() {
        ReferenceController controller;
        ControllerLog log;
        log.attach(controller);
        controller.start(makeRequest(1));
        QVERIFY(!controller.pause()); // Planning 下非法
        QVERIFY(controller.status() == NavigationStatus::Planning);
        QVERIFY(log.has(NavigationEventKind::IllegalEventDropped));

        // 回到 Idle 后再进入导航态（转换表：非 Idle 的 start 会被丢弃）
        controller.stop();
        startNavigating(controller, log);
        QVERIFY(controller.status() == NavigationStatus::Navigating);
        QVERIFY(controller.pause());
        QVERIFY(controller.paused());
        QVERIFY(controller.status() == NavigationStatus::Navigating); // 暂停是子标志，不是独立状态
        QVERIFY(log.has(NavigationEventKind::NavigationPaused));
        QVERIFY(controller.snapshot().paused);
        QVERIFY(controller.resume());
        QVERIFY(!controller.paused());
        QVERIFY(log.has(NavigationEventKind::NavigationResumed));
    }

    // 覆盖转换表 Navigating/locationUpdated + 暂停语义：暂停时只更新位置，不推进剩余距离。
    void locationUpdatesDoNotAdvanceWhilePaused() {
        ReferenceController controller;
        ControllerLog log;
        log.attach(controller);
        controller.setRemainingProvider(linearProvider());
        startNavigating(controller, log);

        controller.locationUpdated(makeSample(1767225601000, 60.000090, 1));
        QVERIFY(controller.snapshot().remainingDistanceM.has_value());
        QVERIFY(std::fabs(*controller.snapshot().remainingDistanceM - 180.3384) < 0.01);
        QCOMPARE(controller.snapshot().currentPosition->timestampMs, std::int64_t(1767225601000));

        QVERIFY(controller.pause());
        const std::size_t snapshotsBefore = static_cast<std::size_t>(log.snapshots);
        controller.locationUpdated(makeSample(1767225602000, 60.000180, 2));
        QCOMPARE(controller.snapshot().currentPosition->timestampMs, std::int64_t(1767225602000)); // 位置仍更新
        QVERIFY(std::fabs(*controller.snapshot().remainingDistanceM - 180.3384) < 0.01);           // 剩余量未推进
        QCOMPARE(static_cast<std::size_t>(log.snapshots), snapshotsBefore);

        QVERIFY(controller.resume());
        controller.locationUpdated(makeSample(1767225603000, 60.000270, 3));
        QVERIFY(std::fabs(*controller.snapshot().remainingDistanceM - 160.3008) < 0.01);
    }

    // 覆盖转换表 Navigating/offRoute + 第 ⑥ 节：偏航转 OffRoute 并发起一次重算；
    // 重复 offRoute 被丢弃；重算在途期间不再发起新请求（10 秒最小间隔同样生效）。
    void offRouteTriggersSingleReroute() {
        ReferenceController controller;
        ControllerLog log;
        log.attach(controller);
        startNavigating(controller, log);
        controller.locationUpdated(makeSample(1767225608000, 60.000720, 8));

        controller.offRoute(8);
        QVERIFY(controller.status() == NavigationStatus::OffRoute);
        QVERIFY(log.has(NavigationEventKind::OffRouteDetected));
        QCOMPARE(log.count(NavigationEventKind::RerouteRequested), 1);
        const std::uint64_t rerouteId = controller.currentRequestId();
        QVERIFY(rerouteId != 0);
        QCOMPARE(log.routeRequests.size(), std::size_t(1));
        QCOMPARE(log.routeRequests[0].requestId, rerouteId);

        // 重复偏航 + 在途重算：不得再发起请求
        controller.offRoute(9);
        controller.locationUpdated(makeSample(1767225609000, 60.000810, 9));
        controller.offRoute(9);
        QCOMPARE(log.count(NavigationEventKind::RerouteRequested), 1);
        QCOMPARE(log.routeRequests.size(), std::size_t(1));
        QVERIFY(log.has(NavigationEventKind::IllegalEventDropped));
    }

    // 覆盖转换表 OffRoute/routeResultReady：重算结果采纳后回到 Navigating（RouteUpdated）。
    void rerouteResultReturnsToNavigating() {
        ReferenceController controller;
        ControllerLog log;
        log.attach(controller);
        startNavigating(controller, log);
        controller.locationUpdated(makeSample(1767225608000, 60.000720, 8));
        controller.offRoute(8);
        const std::uint64_t rerouteId = controller.currentRequestId();

        controller.routeResultReady(makeResult(rerouteId + 1, true)); // 陈旧重算结果
        QVERIFY(controller.status() == NavigationStatus::OffRoute);
        QCOMPARE(log.count(NavigationEventKind::RerouteRejected), 1);

        controller.routeResultReady(makeResult(rerouteId, true));
        QVERIFY(controller.status() == NavigationStatus::Navigating);
        QVERIFY(log.has(NavigationEventKind::RouteUpdated));
        QCOMPARE(controller.currentRequestId(), std::uint64_t(0));
    }

    // 覆盖转换表 Navigating|OffRoute/routeFailed：重算失败不中止导航（继续用旧路线）。
    void rerouteFailureKeepsNavigating() {
        ReferenceController controller;
        ControllerLog log;
        log.attach(controller);
        startNavigating(controller, log);
        controller.locationUpdated(makeSample(1767225608000, 60.000720, 8));
        controller.offRoute(8);
        const std::uint64_t rerouteId = controller.currentRequestId();
        controller.routeFailed(rerouteId, NavigationErrorCode::PlanningFailed, QStringLiteral("stub failure"));
        QVERIFY(controller.status() == NavigationStatus::OffRoute);
        QCOMPARE(log.errors.size(), std::size_t(1));
        QVERIFY(log.errors[0].code == NavigationErrorCode::PlanningFailed);
        QCOMPARE(log.count(NavigationEventKind::NavigationAborted), 0);
        QVERIFY(controller.snapshot().route.has_value()); // 旧路线仍在
    }

    // 覆盖 ④"旧任务异步结果不得覆盖当前导航"：重算后旧 requestId 的结果与失败全部丢弃，新 requestId 采纳。
    void staleResultsNeverOverrideCurrentNavigation() {
        ReferenceController controller;
        ControllerLog log;
        log.attach(controller);
        startNavigating(controller, log);
        controller.locationUpdated(makeSample(1767225608000, 60.000720, 8));

        RerouteRequest request;
        request.requestId = 42;
        request.issuedAtMs = 1767225608000;
        request.currentPosition = GeoCoordinate{CoordinateSystem::WGS84, 60.000720, 116.000360, 50.0};
        request.destination = GeoCoordinate{CoordinateSystem::WGS84, 60.001710, 116.000000, std::nullopt};
        const std::uint64_t rerouteId = controller.requestReroute(request);
        QCOMPARE(rerouteId, std::uint64_t(42));

        controller.routeResultReady(makeResult(1, true)); // 首次规划的旧结果回流
        QVERIFY(controller.status() == NavigationStatus::Navigating);
        QCOMPARE(log.count(NavigationEventKind::RoutePlanRejected), 1);
        controller.routeFailed(1, NavigationErrorCode::PlanningFailed, QStringLiteral("stale failure"));
        QCOMPARE(log.count(NavigationEventKind::RoutePlanRejected), 2);
        QCOMPARE(log.errors.size(), std::size_t(0)); // 旧请求的失败也不产生错误

        controller.routeResultReady(makeResult(42, true)); // 当前重算结果
        QVERIFY(log.has(NavigationEventKind::RouteUpdated));
        QCOMPARE(log.count(NavigationEventKind::RoutePlanRejected), 2);
    }

    // 覆盖转换表 Arrived 行：Navigating/OffRoute → Arrived 为终端态，
    // 到达后的任何异步结果一律丢弃，只能 stop() 回 Idle。
    void arrivedIsTerminalUntilStop() {
        ReferenceController controller;
        ControllerLog log;
        log.attach(controller);
        startNavigating(controller, log);

        controller.arrived(19);
        QVERIFY(controller.status() == NavigationStatus::Arrived);
        QVERIFY(log.has(NavigationEventKind::Arrived));
        QVERIFY(!controller.snapshot().remainingDistanceM.has_value()
                || std::fabs(*controller.snapshot().remainingDistanceM) < 1e-9);
        QCOMPARE(log.guidance.size(), std::size_t(1)); // 切到 ArriveDestination
        QVERIFY(log.guidance[0].kind == GuidanceKind::ArriveDestination);

        controller.routeResultReady(makeResult(77, true)); // 终止态不得被复活
        QVERIFY(controller.status() == NavigationStatus::Arrived);
        QVERIFY(log.has(NavigationEventKind::RoutePlanRejected));
        QVERIFY(!controller.pause());
        QVERIFY(controller.status() == NavigationStatus::Arrived);

        controller.stop();
        QVERIFY(controller.status() == NavigationStatus::Idle);
        QVERIFY(log.has(NavigationEventKind::NavigationStopped));
    }

    // 覆盖转换表 OffRoute/arrived：偏航中到达终点同样结束导航。
    void arrivedFromOffRouteEndsNavigation() {
        ReferenceController controller;
        ControllerLog log;
        log.attach(controller);
        startNavigating(controller, log);
        controller.locationUpdated(makeSample(1767225608000, 60.000720, 8));
        controller.offRoute(8);
        QVERIFY(controller.status() == NavigationStatus::OffRoute);
        controller.arrived(19);
        QVERIFY(controller.status() == NavigationStatus::Arrived);
        QCOMPARE(log.count(NavigationEventKind::Arrived), 1);
    }

    // 覆盖 ⑤ Idle 行：初始 Idle 下 pause/resume/arrived/offRoute 均为非法事件，状态不变。
    void illegalCommandsAreDroppedInIdle() {
        ReferenceController controller;
        ControllerLog log;
        log.attach(controller);
        QVERIFY(!controller.pause());
        QVERIFY(!controller.resume());
        controller.arrived(1);
        controller.offRoute(1);
        controller.routeResultReady(makeResult(1, true));
        controller.routeFailed(1, NavigationErrorCode::PlanningFailed, QStringLiteral("x"));
        QVERIFY(controller.status() == NavigationStatus::Idle);
        QCOMPARE(controller.navigationId(), std::uint64_t(0));
        QCOMPARE(log.count(NavigationEventKind::IllegalEventDropped), 4); // pause/resume/arrived/offRoute
        QCOMPARE(log.count(NavigationEventKind::RoutePlanRejected), 2);   // 结果与失败各一次
    }

    // 覆盖 ③ stop() 语义：stop() 取消在途请求 → Idle；此后为静默期，任何命令都不再产生回调。
    void stopSilencesAllCallbacks() {
        ReferenceController controller;
        ControllerLog log;
        log.attach(controller);
        controller.start(makeRequest(1));
        const std::uint64_t requestId = controller.currentRequestId();
        controller.stop();
        QVERIFY(controller.status() == NavigationStatus::Idle);
        QVERIFY(log.has(NavigationEventKind::NavigationStopped));
        QCOMPARE(log.cancellations.size(), std::size_t(1));
        QCOMPARE(log.cancellations[0], requestId);

        const std::size_t before = log.total();
        controller.locationUpdated(makeSample(1767225601000, 60.000090, 1));
        controller.arrived(2);
        controller.offRoute(3);
        controller.pause();
        controller.resume();
        controller.routeResultReady(makeResult(1, true));
        controller.routeFailed(1, NavigationErrorCode::PlanningFailed, QStringLiteral("x"));
        controller.stop(); // 重复 stop() 为空操作
        QCOMPARE(log.total(), before);
        QVERIFY(controller.status() == NavigationStatus::Idle);
        QVERIFY(!controller.snapshot().route.has_value());
    }
};

QTEST_GUILESS_MAIN(NavigationContractTests)
#include "NavigationContractTests.moc"
