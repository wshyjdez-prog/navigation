#pragma once
// 4 号第 2 次作业：对接层验证用最小控制器桩（RecordingController）。
//
// ⚠️ 这不是第 3 次作业的状态机实现：只实现验证"规划对接链路"所需的最小行为——
// start 发起规划请求、结果按 requestId 采纳（Planning→Navigating）、失败/取消回流、
// 其余命令按转换表记录为丢弃事件。完整状态机以 include/contracts/NavigationController.h
// 第 ⑤ 节转换表为准，属第 3 次作业（测试侧另有覆盖全表的 ReferenceController 脚手架）。
//
// 事件全部记录到 events()，供测试与演示程序断言"哪些事件、什么顺序、几条"。
#include "contracts/NavigationController.h"

#include <QDateTime>
#include <QThread>

#include <vector>

namespace p1::navigation {

class RecordingController final : public p1::contracts::NavigationController {
    Q_OBJECT
public:
    explicit RecordingController(QObject* parent = nullptr) : p1::contracts::NavigationController(parent) {}

    // ── 只读视图 ─────────────────────────────────────────────────────────
    p1::contracts::NavigationSnapshot snapshot() const override {
        p1::contracts::NavigationSnapshot s;
        s.status = status_;
        s.paused = paused_;
        s.navigationId = navigationId_;
        s.currentRequestId = currentRequestId_;
        s.route = route_;
        s.currentPosition = position_;
        s.dataVersion = dataVersion_;
        s.source = p1::contracts::SourceKind::Mock;
        s.updatedAtMs = QDateTime::currentMSecsSinceEpoch();
        return s;
    }
    p1::contracts::NavigationStatus status() const override { return status_; }
    bool paused() const override { return paused_; }
    std::uint64_t navigationId() const override { return navigationId_; }
    std::uint64_t currentRequestId() const override { return currentRequestId_; }

    // 测试观察：全部事件（含被丢弃的）与回调线程标记。
    const std::vector<p1::contracts::NavigationEvent>& events() const { return events_; }
    // 最近一次结果回流是否发生在构造线程（Qt::QueuedConnection 生效的证明）。
    bool lastResultOnControllerThread() const { return lastResultOnControllerThread_; }
    void clearEvents() { events_.clear(); }

    // ── 命令（最小状态机：仅覆盖对接链路涉及的转换） ──────────────────────
    std::uint64_t start(const p1::contracts::RoutePlanRequest& request) override {
        if (status_ == p1::contracts::NavigationStatus::Navigating ||
            status_ == p1::contracts::NavigationStatus::OffRoute ||
            status_ == p1::contracts::NavigationStatus::Arrived) {
            emitEvent(p1::contracts::NavigationEventKind::IllegalEventDropped,
                      QStringLiteral("start 必须先 stop()"));
            return 0;
        }
        // Idle（含 stop() 后）与 Planning 均可发起：Planning 中重新 start 须先取消旧在途请求（转换表）。
        silent_ = false;
        if (status_ == p1::contracts::NavigationStatus::Planning && currentRequestId_ != 0) {
            emit routePlanCancelled(currentRequestId_);
        }
        ++navigationId_;
        currentRequestId_ = request.requestId > 0 ? request.requestId : ++generatedRequestId_;
        dataVersion_ = request.dataVersion;
        status_ = p1::contracts::NavigationStatus::Planning;
        emitEvent(p1::contracts::NavigationEventKind::NavigationStarted, currentRequestId_);
        emitEvent(p1::contracts::NavigationEventKind::RoutePlanningStarted, currentRequestId_);
        emit statusChanged(status_, paused_);
        emit routePlanRequested(request);
        return currentRequestId_;
    }

    bool pause() override {
        emitEvent(p1::contracts::NavigationEventKind::IllegalEventDropped, QStringLiteral("对接验证桩不实现暂停"));
        return false;
    }
    bool resume() override {
        emitEvent(p1::contracts::NavigationEventKind::IllegalEventDropped, QStringLiteral("对接验证桩不实现恢复"));
        return false;
    }

    void stop() override {
        if (silent_ || status_ == p1::contracts::NavigationStatus::Idle) {
            return; // 空操作
        }
        // 取消在途请求 + 唯一一次 NavigationStopped；此后 silent_，残留异步结果静默丢弃（契约 ③）。
        if (currentRequestId_ != 0) {
            emit routePlanCancelled(currentRequestId_);
        }
        const std::uint64_t id = currentRequestId_;
        status_ = p1::contracts::NavigationStatus::Idle;
        currentRequestId_ = 0;
        route_.reset();
        emitEvent(p1::contracts::NavigationEventKind::NavigationStopped, id);
        emit statusChanged(status_, paused_);
        silent_ = true;
    }

    void locationUpdated(const p1::contracts::PositionSample& sample) override {
        position_ = sample; // 只记录位置，不推进引导（推进算法属第 3 次）
    }

    std::uint64_t requestReroute(const p1::contracts::RerouteRequest& request) override {
        emitEvent(p1::contracts::NavigationEventKind::IllegalEventDropped,
                  QStringLiteral("对接验证桩不实现重算（属第 3 次作业）"));
        Q_UNUSED(request);
        return 0;
    }

    void routeResultReady(const p1::contracts::RoutePlanResult& result) override {
        lastResultOnControllerThread_ = (QThread::currentThread() == this->thread());
        if (silent_) {
            return; // 契约 ③：stop() 之后的残留结果静默丢弃，不发事件
        }
        if (result.requestId != currentRequestId_) {
            emitEvent(p1::contracts::NavigationEventKind::RoutePlanRejected, result.requestId,
                      QStringLiteral("stale requestId"));
            return;
        }
        if (!result.route.has_value()) {
            // 首算无路线：中止（转换表 Planning 行）
            emitError(p1::contracts::NavigationErrorCode::NoRouteAvailable, QStringLiteral("stub returned no route"),
                      result.requestId);
            status_ = p1::contracts::NavigationStatus::Idle;
            currentRequestId_ = 0;
            emitEvent(p1::contracts::NavigationEventKind::NavigationAborted, result.requestId);
            emit statusChanged(status_, paused_);
            return;
        }
        // 采纳：Planning → Navigating
        route_ = result.route;
        status_ = p1::contracts::NavigationStatus::Navigating;
        emitEvent(p1::contracts::NavigationEventKind::RoutePlanApplied, result.requestId);
        emit statusChanged(status_, paused_);
        if (!result.instructions.empty()) {
            emit guidanceUpdated(result.instructions.front());
        }
    }

    void routeFailed(std::uint64_t requestId, p1::contracts::NavigationErrorCode code, const QString& message) override {
        if (silent_) {
            return;
        }
        if (requestId != currentRequestId_) {
            emitEvent(p1::contracts::NavigationEventKind::RoutePlanRejected, requestId,
                      QStringLiteral("stale requestId"));
            return;
        }
        if (code == p1::contracts::NavigationErrorCode::PlanningCancelled) {
            // 主动取消：不算故障，等价一次停止
            status_ = p1::contracts::NavigationStatus::Idle;
            currentRequestId_ = 0;
            emitEvent(p1::contracts::NavigationEventKind::NavigationStopped, requestId);
            emit statusChanged(status_, paused_);
            return;
        }
        emitError(code, message, requestId);
        status_ = p1::contracts::NavigationStatus::Idle; // 首算失败无可继续路线：中止
        currentRequestId_ = 0;
        emitEvent(p1::contracts::NavigationEventKind::NavigationAborted, requestId);
        emit statusChanged(status_, paused_);
    }

    void arrived(std::uint64_t locationSeq) override {
        emitEvent(p1::contracts::NavigationEventKind::IllegalEventDropped,
                  QStringLiteral("对接验证桩不实现到达判定（属第 3 次作业）"));
        Q_UNUSED(locationSeq);
    }
    void offRoute(std::uint64_t locationSeq) override {
        emitEvent(p1::contracts::NavigationEventKind::IllegalEventDropped,
                  QStringLiteral("对接验证桩不实现偏航判定（属第 3 次作业）"));
        Q_UNUSED(locationSeq);
    }

private:
    static constexpr std::uint64_t kNoRequest = 0; // 与规划无关的事件 requestId 约定为 0
    void emitEvent(p1::contracts::NavigationEventKind kind, const QString& reason) {
        emitEvent(kind, kNoRequest, reason);
    }
    void emitEvent(p1::contracts::NavigationEventKind kind, std::uint64_t requestId,
                   const QString& reason = QStringLiteral("ok")) {
        p1::contracts::NavigationEvent event;
        event.kind = kind;
        event.status = status_;
        event.paused = paused_;
        event.timestampMs = QDateTime::currentMSecsSinceEpoch();
        event.requestId = requestId;
        event.navigationId = navigationId_;
        event.reason = reason;
        event.source = p1::contracts::SourceKind::Mock;
        events_.push_back(event);
        emit navigationEventOccurred(event);
    }
    void emitError(p1::contracts::NavigationErrorCode code, const QString& message, std::uint64_t requestId) {
        p1::contracts::NavigationError error;
        error.code = code;
        error.message = message;
        error.timestampMs = QDateTime::currentMSecsSinceEpoch();
        error.requestId = requestId;
        error.navigationId = navigationId_;
        error.source = p1::contracts::SourceKind::Mock;
        emit navigationErrorOccurred(error);
    }

    p1::contracts::NavigationStatus status_ = p1::contracts::NavigationStatus::Idle;
    bool paused_ = false;
    bool silent_ = false;
    std::uint64_t navigationId_ = 0;
    std::uint64_t currentRequestId_ = 0;
    std::uint64_t generatedRequestId_ = 0;
    std::optional<p1::contracts::RoutePlanRef> route_;
    std::optional<p1::contracts::PositionSample> position_;
    QString dataVersion_;
    std::vector<p1::contracts::NavigationEvent> events_;
    bool lastResultOnControllerThread_ = false;
};

} // namespace p1::navigation
