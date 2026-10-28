#pragma once
// 导航控制器抽象契约（4 号设计，1 号维护 include/contracts/）。
// 第 1 次作业只定义契约与状态转换表，**不实现状态机**（实现属第 3 次作业，照本表编码）。
// 消费方：5 号渲染（读 NavigationSnapshot）、UI（读 status/paused）、语音（读 GuidanceInstruction）。
//
// ① 线程归属
//   - NavigationController 是 QObject，所有命令(start/pause/resume/stop/locationUpdated/
//     requestReroute/routeResultReady/routeFailed/arrived/offRoute)只能在其所属线程调用，
//     正式工程中即 GUI 主线程。
//   - 信号在控制器所属线程发出。3 号的 RoutePlanner/MapMatcher 若在后台线程计算，
//     必须把结果经 Qt::QueuedConnection 投递回控制器线程再调用 routeResultReady()/routeFailed()。
//   - 信号参数按值复制，不引用控制器内部存储。
//
// ② 所有权与生命周期
//   - 控制器由创建者（UI/应用装配层）独占持有；消费方只连信号，不持有、不删除。
//   - 控制器不持有 LocationSource；只在其所属线程接收 PositionSample。
//   - 析构等价于先 stop()，但不再发出事件；析构后连接由 Qt 自动断开。
//
// ③ stop() 之后不得再产生回调
//   - stop() 同步执行：取消在途规划请求（routePlanCancelled），若当前不是 Idle 则发出且仅发出一次
//     NavigationStopped 事件与 statusChanged(Idle,false)；stop() 返回后不再发出
//     statusChanged/navigationEventOccurred/snapshotUpdated/guidanceUpdated/navigationErrorOccurred，
//     直到下一次 start()。重复 stop() 为空操作。
//   - 排队连接的残留事件仍可能送达，消费方须比较 navigationId/requestId 后丢弃。
//
// ④ 旧任务异步结果不得覆盖当前导航（硬性规则）
//   - 每次 start() 递增 navigationId，每次 start()/requestReroute() 生成新的 requestId（>0，单调递增）。
//   - 只有 requestId == currentRequestId() 的结果才被采纳；其余一律丢弃并发
//     RoutePlanRejected/RerouteRejected 事件，reason 写明"stale requestId"。
//   - routeFailed 同理；PlanningCancelled 只用于主动取消，不算故障。
//
// ════════════════════════════════════════════════════════════════════════════
// ⑤ 状态转换表（完整，第 3 次作业的状态机实现须逐条照此编码）
//    Paused 为 Navigating/OffRoute 的子标志（理由见 NavigationTypes.h），下表用 (paused) 标注。
//    处理策略三类：
//      · 采纳 —— 改变状态并发出对应事件
//      · 丢弃 —— 不改变状态，发 IllegalEventDropped(或 RoutePlanRejected/RerouteRejected)，reason 写明原因
//      · 空操作 —— 不改变状态、不发事件（仅重复 stop()）
// ════════════════════════════════════════════════════════════════════════════
//
// 【Idle】
//   start(request)                 → Planning  采纳：生成 navigationId/requestId，发
//                                              NavigationStarted + RoutePlanningStarted + routePlanRequested
//   locationUpdated(sample)        → Idle      丢弃：未导航，定位源可与导航解耦常开；**不计为错误事件**
//   routeResultReady(result)       → Idle      丢弃：无在途请求，result.requestId 必不等于 currentRequestId(0)
//   routeFailed(id, code, msg)     → Idle      丢弃：同上
//   pause() / resume()             → Idle      丢弃：未导航，谈不上暂停
//   arrived(seq) / offRoute(seq)   → Idle      丢弃：无路线，偏航/到达无意义
//   requestReroute(req)            → Idle      丢弃：无当前路线可重算
//   stop()                         → Idle      空操作
//
// 【Planning】
//   routeResultReady(当前 requestId，route 有效) → Navigating  采纳：RoutePlanApplied，
//                                              填充 route/剩余距离时间/引导(index=0)，发 GuidanceUpdated
//   routeResultReady(当前 requestId，route 空)   → Idle        中止：navigationErrorOccurred(NoRouteAvailable)
//                                              + NavigationAborted。理由：首算失败没有可继续的旧路线
//   routeResultReady(非当前 requestId)          → Planning    丢弃：RoutePlanRejected，reason="stale requestId"
//   routeFailed(当前 requestId, 非 Cancelled)   → Idle        中止：navigationErrorOccurred(PlanningFailed)
//                                              + NavigationAborted
//   routeFailed(当前 requestId, PlanningCancelled) → Idle     采纳取消：NavigationStopped，不算故障
//   routeFailed(非当前 requestId, 任意)          → Planning   丢弃：RoutePlanRejected
//   start(新 request)              → Planning  采纳：先 routePlanCancelled(旧 requestId)，
//                                              再生成新 requestId 重新规划；navigationId 递增
//   locationUpdated(sample)        → Planning  采纳但不改状态：记录为最新位置，供规划成功后立即使用
//   stop()                         → Idle      采纳：取消在途请求 + NavigationStopped
//   pause() / resume()             → Planning  丢弃：规划未完成时不能暂停
//   arrived(seq) / offRoute(seq)   → Planning  丢弃：尚无路线，无法判定到达/偏航
//   requestReroute(req)            → Planning  丢弃：已有在途首算请求，交由去重策略（第 3 次）处理
//
// 【Navigating】(paused 可为 true/false)
//   locationUpdated(sample)        → Navigating 采纳：更新 currentPosition；paused=false 时推进引导与
//                                              剩余距离时间（推进算法属第 3 次），paused=true 时只更新位置不推进
//   pause()                        → Navigating(paused=true)   采纳：NavigationPaused
//   resume()                       → Navigating(paused=false)  采纳：NavigationResumed
//   offRoute(seq)                  → OffRoute  采纳：OffRouteDetected，随后按第 ⑥ 节已确认的重算去重策略
//                                              决定是否 RerouteRequested（偏航判定策略同样见第 ⑥ 节）
//   arrived(seq)                   → Arrived   采纳：Arrived
//   requestReroute(req)            → Navigating 采纳：生成新 requestId，发 RerouteRequested +
//                                              routePlanRequested；旧请求作废
//   routeResultReady(当前 requestId) → Navigating 采纳：RouteUpdated（主动重算结果）
//   routeResultReady(非当前 requestId) → Navigating 丢弃：RoutePlanRejected，reason="stale requestId"
//   routeFailed(当前 requestId, 任意) → Navigating 采纳错误但不改状态：navigationErrorOccurred，
//                                              **继续用旧路线导航**。理由：重算失败不应打断进行中的导航
//   routeFailed(非当前 requestId)     → Navigating 丢弃：RerouteRejected
//   start(新 request)              → Navigating 丢弃：必须先 stop()。理由：避免隐式续接导致 navigationId 混乱
//   stop()                         → Idle      采纳：NavigationStopped
//
// 【OffRoute】
//   locationUpdated(sample)        → OffRoute  采纳：更新位置；回归判定由第 3 次实现
//   offRoute(seq) 重复             → OffRoute  丢弃：IllegalEventDropped，reason="already off-route"。
//                                              理由：避免重复触发重算（去重规则见第 ⑥ 节，已确认）
//   routeResultReady(当前 requestId) → Navigating 采纳：RouteUpdated（视为已回归）
//   routeResultReady(非当前 requestId) → OffRoute 丢弃：RerouteRejected
//   routeFailed(当前 requestId)     → OffRoute  采纳错误但不改状态：保持偏航提示，等待下次重算
//   arrived(seq)                   → Arrived   采纳：Arrived。理由：偏航中到达终点同样应结束导航
//   pause() / resume()             → OffRoute(paused 切换) 采纳：暂停不改变偏航状态
//   requestReroute(req)            → OffRoute  采纳：新 requestId 取代旧请求
//   start(新 request)              → OffRoute  丢弃：必须先 stop()
//   stop()                         → Idle      采纳：NavigationStopped
//
// 【Arrived】（终端态）
//   locationUpdated(sample)        → Arrived   采纳但不推进：只更新位置。理由：到达后不再需要引导
//   stop()                         → Idle      采纳：NavigationStopped
//   start(新 request)              → Arrived   丢弃：必须先 stop()。理由：避免未结束的导航被隐式续接
//   pause() / resume()             → Arrived   丢弃：已结束，暂停无意义
//   arrived(seq)                   → Arrived   丢弃：IllegalEventDropped，reason="already arrived"
//   offRoute(seq)                  → Arrived   丢弃：已到达，不再判定偏航
//   routeResultReady / routeFailed → Arrived   丢弃（一律）。理由：终止态不得被任何异步结果复活
//   requestReroute(req)            → Arrived   丢弃：同上
//
// 【跨状态·定位丢失】
//   长时间 NoFix（判定阈值属第 3 次）→ Idle  中止：navigationErrorOccurred(AbortedByLocationLost)
//                                              + NavigationAborted。理由：无定位则无法继续导航结论
//   短时 NoFix                      → 保持当前状态，等待恢复；不作废路线
//
// ════════════════════════════════════════════════════════════════════════════
// ⑥ 已确认的偏航判定与重算去重策略（用户 2026-10-28 从三个候选中选定"候选 2"）
//    第 3 次作业的状态机实现须遵守本节；样例按此构造（tests/fixtures/location/）。
//    · 偏航判定：横向偏离路线 **> 15 米** 且 **持续 ≥ 3 秒**（至少 3 个连续帧）才判定偏航，
//      调用 offRoute(seq)。单次超阈值不判定，避免 GNSS 抖动误判。
//      （偏离量本身由 3 号 MapMatcher 给出，本契约只规定阈值与持续时间。）
//    · 重算最小间隔：**10 秒**。距上一次重算不足 10 秒不再发起新请求。
//    · 重算在途：已有在途 requestId 时，新位置 **不触发** 新的规划请求，
//      必须等 routeResultReady()/routeFailed() 回流后再判断。
//    · 间隔按**定位样本的时间戳**计算（不按墙钟），保证回放测试确定性。
//    尚未确定（仍属第 3 次作业）：到达半径、长时间无定位的时长阈值、精度过低阈值。
// ════════════════════════════════════════════════════════════════════════════
#include "contracts/NavigationTypes.h"

#include <QObject>

namespace p1::contracts {

// 导航控制器抽象接口。实现类（第 3 次作业）须遵守文件头的线程、所有权、停止与陈旧结果规则。
// 注意：本头文件含 Q_OBJECT，其 moc 产物编译进 p1_navigation 库；使用方链接 p1_navigation。
class NavigationController : public QObject {
    Q_OBJECT
public:
    explicit NavigationController(QObject* parent = nullptr) : QObject(parent) {}
    ~NavigationController() override = default;

    // ── 只读视图 ──────────────────────────────────────────────────────────
    virtual NavigationSnapshot snapshot() const = 0;      // 聚合快照（只读、按值返回）
    virtual NavigationStatus status() const = 0;          // 当前状态
    virtual bool paused() const = 0;                      // 暂停子标志
    virtual std::uint64_t navigationId() const = 0;       // 本次导航会话 ID；从未启动为 0
    virtual std::uint64_t currentRequestId() const = 0;   // 当前有效的规划请求 ID；0 表示无在途请求

    // ── 命令（同步调用，异步结果通过 routeResultReady/routeFailed 回流） ────
    // 开始一次导航：Idle → Planning，生成 navigationId 与 requestId 并发起规划。
    // 非 Idle 时按转换表丢弃并返回 0。
    virtual std::uint64_t start(const RoutePlanRequest& request) = 0;
    // 暂停：仅 Navigating/OffRoute 有效，返回是否改变；暂停时不推进引导但仍在接收定位。
    virtual bool pause() = 0;
    // 恢复：仅 paused==true 时有效，返回是否改变。
    virtual bool resume() = 0;
    // 停止：任意态 → Idle，取消在途请求；返回后不再回调（见文件头 ③）。
    virtual void stop() = 0;
    // 定位样本入口。会话/来源校验由实现做；陈旧 sessionId 的样本应被丢弃。
    virtual void locationUpdated(const PositionSample& sample) = 0;
    // 请求重算：生成新 requestId 取代旧请求；返回新 requestId，非法时返回 0。
    virtual std::uint64_t requestReroute(const RerouteRequest& request) = 0;
    // 3 号 RoutePlanner 的结果回流：仅 requestId == currentRequestId() 时被采纳。
    virtual void routeResultReady(const RoutePlanResult& result) = 0;
    // 3 号 RoutePlanner 的失败回流；PlanningCancelled 表示主动取消，非故障。
    virtual void routeFailed(std::uint64_t requestId, NavigationErrorCode code, const QString& message) = 0;
    // 到达终点：由引导/匹配层判定后调用，携带触发的定位样本 seq。
    virtual void arrived(std::uint64_t locationSeq) = 0;
    // 偏航：由匹配层判定后调用，携带触发的定位样本 seq；去重策略属第 3 次作业。
    virtual void offRoute(std::uint64_t locationSeq) = 0;

signals:
    // 状态或暂停子标志变化（含 stop() 后的 Idle）。
    void statusChanged(p1::contracts::NavigationStatus status, bool paused);
    // 每个命令/异步结果对应的事件记录；被丢弃的也发，便于契约测试断言。
    void navigationEventOccurred(const p1::contracts::NavigationEvent& event);
    // 当前引导指令变化（index 或文本变化时）。
    void guidanceUpdated(const p1::contracts::GuidanceInstruction& instruction);
    // 快照更新（位置推进、剩余距离时间变化等）。
    void snapshotUpdated(const p1::contracts::NavigationSnapshot& snapshot);
    // 需要 UI 提示或导致中止的失败。
    void navigationErrorOccurred(const p1::contracts::NavigationError& error);
    // 向 3 号 RoutePlanner 发出的规划请求（本契约不实现规划，只规定调用形状）。
    void routePlanRequested(const p1::contracts::RoutePlanRequest& request);
    void routePlanCancelled(std::uint64_t requestId);
    // 向 3 号 MapMatcher 发出的匹配请求（偏航/回归判定由其结果驱动）。
    void mapMatchRequested(const p1::contracts::MapMatchRequest& request);
};

// 3 号 RoutePlanner 的调用形状（异步 requestId）。
// 第 1 次作业只用本地 stub：stub 直接同步/定时回吐 routeReady/routeFailed，不接入 A*/HMM。
// 替换点：第 2 次作业替换为 3 号真实实现，控制器侧无需改动（只依赖本接口）。
class RoutePlanner : public QObject {
    Q_OBJECT
public:
    explicit RoutePlanner(QObject* parent = nullptr) : QObject(parent) {}
    ~RoutePlanner() override = default;

    // 发起规划，返回 requestId（>0）；输入非法时返回 0。
    virtual std::uint64_t requestRoute(const RoutePlanRequest& request) = 0;
    // 取消指定请求；取消后不得再回吐该 requestId 的结果。
    virtual void cancel(std::uint64_t requestId) = 0;

signals:
    void routeReady(const p1::contracts::RoutePlanResult& result);
    void routeFailed(std::uint64_t requestId, p1::contracts::NavigationErrorCode code, const QString& message);
};

// 3 号 MapMatcher 的调用形状（异步 requestId）。
// 第 1 次作业只用本地 stub：不实现 HMM，不判定偏航阈值。
class MapMatcher : public QObject {
    Q_OBJECT
public:
    explicit MapMatcher(QObject* parent = nullptr) : QObject(parent) {}
    ~MapMatcher() override = default;

    // 提交一次匹配，返回 requestId（>0）。
    virtual std::uint64_t requestMatch(const MapMatchRequest& request) = 0;

signals:
    void matchReady(const p1::contracts::MapMatchResult& result);
};

} // namespace p1::contracts
