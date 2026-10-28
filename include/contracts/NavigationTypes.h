#pragma once
// 导航状态与事件公共契约（4 号设计，1 号维护 include/contracts/）。
//
// 适用范围（重要，避免越界）：
//   - 4 号第 1 次作业只**定义契约**：类型、事件、转换表、错误码、3 号调用形状。
//     状态机实现、偏航判断算法、重算去重策略均属第 3 次作业，本次**不实现**。
//   - 路网几何格式属 2/3 号范围，本契约只**引用**不定义（见 RoutePlanRef::geometryRef）。
//   - 不接真实 G6 / FFI（X06 未确认），第 4 次作业再联调。
//
// 统一约定：距离米、时长秒、速度米/秒、角度度、时间戳 Unix 毫秒(int64_t)；
//           全部带 requestId/会话标识、数据版本、mock/real 来源标记；
//           经纬度必须带坐标系标记（见 LocationTypes.h）；
//           旧任务异步结果不得覆盖当前导航（以 requestId 判定并丢弃）。
// 核心数据结构只用 C++17 标准库（std::optional / std::vector）；
// Qt 仅用于 QString 与信号槽元类型注册。仅使用 Qt 6.4.2 已有 API。
//
// ⑦ 与 3 号算法模型的坐标边界（2026-10-28 依据 3 号 P0 模块 src/algorithms/model/Types.h 确定）
//   3 号内部模型使用**米制平面坐标**（CoordinateFrame: LocalCartesian / ProjectedCartesian，
//   LengthUnit::Metres），其 README 明确要求"WGS84 经纬度须先由适配层转换，不能将角度直接填为米"。
//   本契约据此确定分工（不改任何一方的内部表示）：
//   · 4 号**只传带坐标系标记的经纬度（度）**，不做任何投影或米制换算。理由：4 号不掌握数据集的
//     投影参数与局部原点（这些属 2/3 号的 CoordinateFrame::definition），由 4 号换算即越权，
//     且会让导航契约依赖具体数据集，切换数据集时全部失效。
//   · 度 → 米的转换由 3 号适配层（配合 2 号确定坐标框架与参数）在 RoutePlanner / MapMatcher
//     实现内部完成；4 号契约与状态机不假设任何投影、原点或比例尺。
//   · 反向：MapMatchResult 的 distanceToRouteM / alongRouteDistanceM、RoutePlanRef 的
//     totalDistanceM / totalDurationSec 一律以**米 / 秒**返回，由 3 号在其坐标框架内算好后给出，
//     4 号不再二次换算（偏航判定直接用这些米值，见 NavigationController.h 第 ⑥ 节）。
//   若 3 号后续要求调用方直接传米制坐标，属接口变更，须经 3 号/1 号确认后同步修改本文件、
//   docs/navigation-contracts.md 第 6.1 节与相关测试。
#include "contracts/LocationTypes.h"

#include <QString>
#include <cstdint>
#include <optional>
#include <vector>

namespace p1::contracts {

// ─────────────────────────────────────────────────────────────────────────────
// 导航状态（五态，与 project.md 第 6 节"第 1 次只定契约"一致）
//
// 关于 Paused 的表达方式 —— 本契约决定：**Paused 不作为独立状态，而是 Navigating 的子标志**
// （NavigationSnapshot::paused 与 statusChanged(status, paused) 第二个参数）。理由：
//   1. 暂停不改变"当前是否持有有效路线并正在导航"这一状态本质，只是停止推进引导；
//      若设为独立状态，则 Paused 与 OffRoute/Planning 会组合出 7+ 个状态，转换表规模翻倍。
//   2. 暂停期间定位仍在更新、路线仍有效，UI/5 号渲染只需读 snapshot.paused 即可区分，
//      不需要在状态枚举里再判一次。
//   3. 保持五态枚举与作业书一致，5 号渲染、G6 事件映射的分支数量可控。
// 代价：所有状态判断都要同时看 (status, paused)，已在 NavigationSnapshot 中集中提供。
// 若第 3 次作业实现时需要 Paused 独立成状态，属状态机实现细节，可在本契约之上扩展，
// 但须同步修改本文件注释与 docs/navigation-contracts.md 的转换表。
// ─────────────────────────────────────────────────────────────────────────────
enum class NavigationStatus : std::uint8_t {
    Idle,       // 未导航：无有效路线。start() 前与 stop() 后的状态
    Planning,   // 已发起路线规划，等待 3 号 RoutePlanner 结果
    Navigating, // 有有效路线，正在导航（paused 子标志可为 true）
    OffRoute,   // 偏航：仍持有旧路线，等待/正在重算（偏航判定属第 3 次作业）
    Arrived     // 已到达终点：终端态，只能 stop() 回 Idle
};

// 导航事件种类。每个命令/异步结果最终都落成这里的一个事件，供日志、测试与 5 号渲染消费。
enum class NavigationEventKind : std::uint8_t {
    NavigationStarted,    // start() 被接受：Idle → Planning
    RoutePlanningStarted, // 已向 RoutePlanner 发出带 requestId 的规划请求
    RoutePlanApplied,     // 规划结果被采纳：Planning → Navigating
    RoutePlanRejected,    // 规划结果被丢弃（requestId 陈旧或状态不匹配），状态不变
    NavigationPaused,     // Navigating 的 paused 子标志置 true
    NavigationResumed,    // paused 子标志置 false
    OffRouteDetected,     // Navigating → OffRoute
    RerouteRequested,     // 偏航后发起重算（是否/何时发起由第 3 次作业的去重策略决定）
    RouteUpdated,         // 重算结果被采纳：OffRoute → Navigating
    RerouteRejected,      // 重算结果被丢弃（陈旧 requestId 或状态不匹配）
    GuidanceUpdated,      // 当前引导指令发生变化
    Arrived,              // Navigating/OffRoute → Arrived
    NavigationStopped,    // 任意态 → Idle（显式 stop()）
    NavigationAborted,    // 异常中止 → Idle（无可用路线/规划失败/定位丢失）
    IllegalEventDropped   // 非法事件或命令被丢弃：状态不变，原因写入 reason
};

// 导航错误码（区分四类需要不同处理方式的失败）
enum class NavigationErrorCode : std::uint8_t {
    NoRouteAvailable,     // 无可用路线：起终点不可达或路网无解，中止导航
    PlanningFailed,       // 计算失败：规划器内部错误；重算失败时不中止，继续用旧路线
    PlanningCancelled,    // 计算取消：因 stop()/新 start() 主动取消，非故障
    AbortedByLocationLost // 定位丢失导致中止：位置源长时间 NoFix
};

// 引导动作类型（文本由第 3 次作业的引导生成模块填充；类型集合可按需增补）
enum class GuidanceKind : std::uint8_t {
    Unknown,
    Depart,            // 出发
    ContinueStraight,  // 沿当前道路直行
    TurnLeft,          // 左转
    TurnRight,         // 右转
    SlightLeft,        // 稍向左
    SlightRight,       // 稍向右
    SharpLeft,         // 向左急转
    SharpRight,        // 向右急转
    UTurn,             // 掉头
    KeepLeft,          // 靠左
    KeepRight,         // 靠右
    RoundaboutEnter,   // 进入环岛
    RoundaboutExit,    // 驶出环岛
    Merge,             // 汇入主路
    ArriveDestination  // 到达目的地
};

// 单条引导指令。
// | 字段               | 类型         | 单位 | 可选 | 说明                                              |
// | kind               | GuidanceKind | —    | 否   | 动作类型；未识别时填 Unknown，不得省略            |
// | text               | QString      | —    | 否   | 面向用户的引导文本（语音/UI 共用）                |
// | distanceToManeuverM| double       | 米   | 否   | 从当前位置到该动作执行点的沿路线距离，>= 0        |
// | index              | uint32_t     | 序号 | 否   | 引导序号，在一次导航内从 0 起单调递增且不重复；   |
// |                    |              |      |      | 重算后沿用新路线自身的序号，供测试断言与去重      |
struct GuidanceInstruction {
    GuidanceKind kind = GuidanceKind::Unknown;
    QString text;
    double distanceToManeuverM = 0.0;
    std::uint32_t index = 0;
};

// 路线引用（**只引用，不定义路网几何格式**）。
// | 字段             | 类型       | 单位 | 可选 | 说明                                                          |
// | routeId          | QString    | —    | 否   | 3 号规划器给出的路线标识，本契约只透传不解释其结构            |
// | dataVersion      | QString    | —    | 否   | 路线数据版本；与 PositionSample::dataVersion 分开记录          |
// | geometryRef      | QString    | —    | 否   | 几何引用（不透明字符串）。**格式由 2/3 号定义**，本契约不定义  |
// | totalDistanceM   | double     | 米   | 否   | 路线总长度，>= 0                                              |
// | totalDurationSec | double     | 秒   | 否   | 路线预计总时长，>= 0                                          |
// | source           | SourceKind | —    | 否   | mock(本地 stub 生成的参考路线)/real                           |
// | requestId        | uint64_t   | —    | 否   | 产生该路线的规划请求 ID，用于陈旧结果判定                     |
struct RoutePlanRef {
    QString routeId;
    QString dataVersion;
    QString geometryRef;
    double totalDistanceM = 0.0;
    double totalDurationSec = 0.0;
    SourceKind source = SourceKind::Mock;
    std::uint64_t requestId = 0;
};

// 规划请求（输入形状）。只描述"给 3 号什么"，不替 3 号选算法或定路网格式。
// | 字段        | 类型           | 单位           | 可选 | 说明                                                |
// | requestId   | uint64_t       | —              | 否   | 由调用方生成的单调递增请求 ID（>0）                  |
// | issuedAtMs  | int64_t        | Unix 毫秒(UTC) | 否   | 请求发出时刻（墙钟）                                |
// | origin      | GeoCoordinate  | 度             | 否   | 起点（带坐标系）                                    |
// | destination | GeoCoordinate  | 度             | 否   | 终点（带坐标系；与 origin 坐标系不一致时应被拒绝）   |
// | dataVersion | QString        | —              | 否   | 请求方使用的数据版本                                |
// | options     | QString        | —              | 是   | 不透明的规划参数（如偏好）；语义由 3 号定义，本契约不解释 |
struct RoutePlanRequest {
    std::uint64_t requestId = 0;
    std::int64_t issuedAtMs = 0;
    GeoCoordinate origin;
    GeoCoordinate destination;
    QString dataVersion;
    std::optional<QString> options;
};

// 规划结果（输出形状）。异步回流时由 requestId 判定是否为当前请求。
// | 字段         | 类型                          | 单位 | 可选 | 说明                                    |
// | requestId    | uint64_t                      | —    | 否   | 对应的请求 ID                            |
// | route        | std::optional<RoutePlanRef>   | —    | 是   | 成功时为路线引用，失败时为空              |
// | instructions | std::vector<GuidanceInstruction> | — | 否   | 引导指令序列（按 index 升序）；失败时为空 |
struct RoutePlanResult {
    std::uint64_t requestId = 0;
    std::optional<RoutePlanRef> route;
    std::vector<GuidanceInstruction> instructions;
};

// 重算请求（偏航后的再规划输入）。与 RoutePlanRequest 分开：重算的起点通常是当前定位点。
// | 字段            | 类型          | 单位           | 可选 | 说明                                          |
// | requestId       | uint64_t      | —              | 否   | 新的单调递增请求 ID（>0，取代旧请求）          |
// | issuedAtMs      | int64_t       | Unix 毫秒(UTC) | 否   | 请求发出时刻                                  |
// | currentPosition | GeoCoordinate | 度             | 否   | 当前定位点（带坐标系）                        |
// | destination     | GeoCoordinate | 度             | 否   | 原终点                                        |
// | previousRouteId | QString       | —              | 是   | 被替换的路线 ID，便于日志追踪                  |
// | locationSeq     | uint64_t      | 序号           | 是   | 触发本次重算的定位样本 seq                     |
struct RerouteRequest {
    std::uint64_t requestId = 0;
    std::int64_t issuedAtMs = 0;
    GeoCoordinate currentPosition;
    GeoCoordinate destination;
    std::optional<QString> previousRouteId;
    std::optional<std::uint64_t> locationSeq;
};

// 地图匹配请求（MapMatcher 的输入形状，3 号实现）。**本契约不定义匹配算法与结果精度**。
// | 字段        | 类型          | 单位 | 可选 | 说明                        |
// | requestId   | uint64_t      | —    | 否   | 单调递增请求 ID（>0）        |
// | position    | GeoCoordinate | 度   | 否   | 待匹配的原始定位点（度，带坐标系；米制换算由 3 号适配层负责，见文件头 ⑦） |
// | locationSeq | uint64_t      | 序号 | 否   | 该定位样本的 seq            |
// | routeId     | QString       | —    | 是   | 当前路线 ID；为空表示无路线  |
struct MapMatchRequest {
    std::uint64_t requestId = 0;
    GeoCoordinate position;
    std::uint64_t locationSeq = 0;
    std::optional<QString> routeId;
};

// 地图匹配结果。**字段语义与阈值由 3 号定义**，本契约只规定可选性与"不得默认 onRoute"
// （未给出判定结果时消费方不得假定在路线上）。
// | 字段                | 类型                    | 单位 | 可选 | 说明                                    |
// | requestId          | uint64_t                | —    | 否   | 对应的请求 ID                            |
// | locationSeq        | uint64_t                | 序号 | 否   | 对应的定位样本 seq                       |
// | matched            | std::optional<GeoCoordinate> | 度 | 是 | 匹配到路网上的点；为空表示匹配失败        |
// | distanceToRouteM   | std::optional<double>   | 米   | 是   | 到路线的偏离距离；语义/阈值由 3 号定义    |
// | alongRouteDistanceM| std::optional<double>   | 米   | 是   | 沿路线已行驶距离；用于推算剩余距离        |
struct MapMatchResult {
    std::uint64_t requestId = 0;
    std::uint64_t locationSeq = 0;
    std::optional<GeoCoordinate> matched;
    std::optional<double> distanceToRouteM;
    std::optional<double> alongRouteDistanceM;
};

// 导航事件。每个命令/异步结果至少产生一个事件，供日志、契约测试与 5 号渲染消费。
// | 字段        | 类型                          | 单位           | 可选 | 说明                                            |
// | kind        | NavigationEventKind           | —              | 否   | 事件种类                                        |
// | status      | NavigationStatus              | —              | 否   | 事件发生后的状态（被丢弃时为原状态）            |
// | paused      | bool                          | —              | 否   | 事件发生后的暂停子标志                          |
// | timestampMs | int64_t                       | Unix 毫秒(UTC) | 否   | 事件触发时刻（墙钟）                            |
// | locationSeq | std::optional<uint64_t>       | 序号           | 是   | 关联的定位样本 seq（与位置有关的事件才有）      |
// | requestId   | uint64_t                      | —              | 否   | 关联的规划请求 ID；0 表示与规划无关            |
// | navigationId| uint64_t                      | —              | 否   | 本次导航会话标识，每次 start() 递增              |
// | reason      | QString                       | —              | 否   | 可读原因；被丢弃/失败时必须写明原因            |
// | source      | SourceKind                    | —              | 否   | mock/real 来源标记                              |
struct NavigationEvent {
    NavigationEventKind kind = NavigationEventKind::IllegalEventDropped;
    NavigationStatus status = NavigationStatus::Idle;
    bool paused = false;
    std::int64_t timestampMs = 0;
    std::optional<std::uint64_t> locationSeq;
    std::uint64_t requestId = 0;
    std::uint64_t navigationId = 0;
    QString reason;
    SourceKind source = SourceKind::Mock;
};

// 导航错误（与事件分开：错误是需要 UI 提示或需要中止导航的失败，事件只是状态变化的记录）。
// | 字段        | 类型                | 单位           | 可选 | 说明                        |
// | code        | NavigationErrorCode | —              | 否   | 错误类别                    |
// | message     | QString             | —              | 否   | 面向开发者的可读说明        |
// | timestampMs | int64_t             | Unix 毫秒(UTC) | 否   | 错误发生时刻                |
// | requestId   | uint64_t            | —              | 否   | 关联的规划请求 ID；0 表示无关 |
// | navigationId| uint64_t            | —              | 否   | 本次导航会话标识            |
// | source      | SourceKind          | —              | 否   | mock/real 来源标记          |
struct NavigationError {
    NavigationErrorCode code = NavigationErrorCode::PlanningFailed;
    QString message;
    std::int64_t timestampMs = 0;
    std::uint64_t requestId = 0;
    std::uint64_t navigationId = 0;
    SourceKind source = SourceKind::Mock;
};

// 导航快照：5 号渲染与 UI 每帧消费的唯一聚合视图（只读、按值复制）。
// | 字段                 | 类型                                | 单位           | 可选 | 说明                                        |
// | status               | NavigationStatus                    | —              | 否   | 当前状态                                    |
// | paused               | bool                                | —              | 否   | 暂停子标志，仅 Navigating/OffRoute 有意义    |
// | navigationId         | uint64_t                            | —              | 否   | 本次导航会话标识，每次 start() 递增          |
// | currentRequestId     | uint64_t                            | —              | 否   | 当前有效的规划请求 ID；0 表示无在途请求      |
// | route                | std::optional<RoutePlanRef>         | —              | 是   | 当前路线；Idle 时为空                        |
// | currentPosition      | std::optional<PositionSample>       | —              | 是   | 最近一次被采纳的定位样本；空表示尚无定位     |
// | remainingDistanceM   | std::optional<double>               | 米             | 是   | 到终点的剩余距离；无路线/无定位时为空        |
// | remainingDurationSec | std::optional<double>               | 秒             | 是   | 预计剩余时长                                |
// | guidance             | std::optional<GuidanceInstruction>  | —              | 是   | 当前引导指令                                |
// | guidanceIndex        | std::optional<uint32_t>             | 序号           | 是   | 当前引导指令的 index（与 guidance 同步）     |
// | dataVersion          | QString                             | —              | 否   | 快照的数据版本（导航侧）                    |
// | source               | SourceKind                          | —              | 否   | mock/real 来源标记                          |
// | updatedAtMs          | int64_t                             | Unix 毫秒(UTC) | 否   | 快照生成时刻                                |
struct NavigationSnapshot {
    NavigationStatus status = NavigationStatus::Idle;
    bool paused = false;
    std::uint64_t navigationId = 0;
    std::uint64_t currentRequestId = 0;
    std::optional<RoutePlanRef> route;
    std::optional<PositionSample> currentPosition;
    std::optional<double> remainingDistanceM;
    std::optional<double> remainingDurationSec;
    std::optional<GuidanceInstruction> guidance;
    std::optional<std::uint32_t> guidanceIndex;
    QString dataVersion;
    SourceKind source = SourceKind::Mock;
    std::int64_t updatedAtMs = 0;
};

inline const char* toString(NavigationStatus status) {
    switch (status) {
    case NavigationStatus::Idle: return "Idle";
    case NavigationStatus::Planning: return "Planning";
    case NavigationStatus::Navigating: return "Navigating";
    case NavigationStatus::OffRoute: return "OffRoute";
    case NavigationStatus::Arrived: return "Arrived";
    }
    return "?";
}

inline const char* toString(NavigationEventKind kind) {
    switch (kind) {
    case NavigationEventKind::NavigationStarted: return "NavigationStarted";
    case NavigationEventKind::RoutePlanningStarted: return "RoutePlanningStarted";
    case NavigationEventKind::RoutePlanApplied: return "RoutePlanApplied";
    case NavigationEventKind::RoutePlanRejected: return "RoutePlanRejected";
    case NavigationEventKind::NavigationPaused: return "NavigationPaused";
    case NavigationEventKind::NavigationResumed: return "NavigationResumed";
    case NavigationEventKind::OffRouteDetected: return "OffRouteDetected";
    case NavigationEventKind::RerouteRequested: return "RerouteRequested";
    case NavigationEventKind::RouteUpdated: return "RouteUpdated";
    case NavigationEventKind::RerouteRejected: return "RerouteRejected";
    case NavigationEventKind::GuidanceUpdated: return "GuidanceUpdated";
    case NavigationEventKind::Arrived: return "Arrived";
    case NavigationEventKind::NavigationStopped: return "NavigationStopped";
    case NavigationEventKind::NavigationAborted: return "NavigationAborted";
    case NavigationEventKind::IllegalEventDropped: return "IllegalEventDropped";
    }
    return "?";
}

inline const char* toString(NavigationErrorCode code) {
    switch (code) {
    case NavigationErrorCode::NoRouteAvailable: return "NoRouteAvailable";
    case NavigationErrorCode::PlanningFailed: return "PlanningFailed";
    case NavigationErrorCode::PlanningCancelled: return "PlanningCancelled";
    case NavigationErrorCode::AbortedByLocationLost: return "AbortedByLocationLost";
    }
    return "?";
}

inline const char* toString(GuidanceKind kind) {
    switch (kind) {
    case GuidanceKind::Unknown: return "Unknown";
    case GuidanceKind::Depart: return "Depart";
    case GuidanceKind::ContinueStraight: return "ContinueStraight";
    case GuidanceKind::TurnLeft: return "TurnLeft";
    case GuidanceKind::TurnRight: return "TurnRight";
    case GuidanceKind::SlightLeft: return "SlightLeft";
    case GuidanceKind::SlightRight: return "SlightRight";
    case GuidanceKind::SharpLeft: return "SharpLeft";
    case GuidanceKind::SharpRight: return "SharpRight";
    case GuidanceKind::UTurn: return "UTurn";
    case GuidanceKind::KeepLeft: return "KeepLeft";
    case GuidanceKind::KeepRight: return "KeepRight";
    case GuidanceKind::RoundaboutEnter: return "RoundaboutEnter";
    case GuidanceKind::RoundaboutExit: return "RoundaboutExit";
    case GuidanceKind::Merge: return "Merge";
    case GuidanceKind::ArriveDestination: return "ArriveDestination";
    }
    return "?";
}

} // namespace p1::contracts

Q_DECLARE_METATYPE(p1::contracts::NavigationStatus)
Q_DECLARE_METATYPE(p1::contracts::NavigationEventKind)
Q_DECLARE_METATYPE(p1::contracts::NavigationErrorCode)
Q_DECLARE_METATYPE(p1::contracts::GuidanceKind)
Q_DECLARE_METATYPE(p1::contracts::GuidanceInstruction)
Q_DECLARE_METATYPE(p1::contracts::RoutePlanRef)
Q_DECLARE_METATYPE(p1::contracts::RoutePlanRequest)
Q_DECLARE_METATYPE(p1::contracts::RoutePlanResult)
Q_DECLARE_METATYPE(p1::contracts::RerouteRequest)
Q_DECLARE_METATYPE(p1::contracts::MapMatchRequest)
Q_DECLARE_METATYPE(p1::contracts::MapMatchResult)
Q_DECLARE_METATYPE(p1::contracts::NavigationEvent)
Q_DECLARE_METATYPE(p1::contracts::NavigationError)
Q_DECLARE_METATYPE(p1::contracts::NavigationSnapshot)

namespace p1::contracts {
// 跨线程排队连接前调用一次（重复调用无害）。须位于 Q_DECLARE_METATYPE 之后。
inline void registerNavigationMetaTypes() {
    qRegisterMetaType<p1::contracts::NavigationStatus>();
    qRegisterMetaType<p1::contracts::NavigationEventKind>();
    qRegisterMetaType<p1::contracts::NavigationEvent>();
    qRegisterMetaType<p1::contracts::NavigationError>();
    qRegisterMetaType<p1::contracts::NavigationSnapshot>();
    qRegisterMetaType<p1::contracts::GuidanceInstruction>();
    qRegisterMetaType<p1::contracts::RoutePlanRef>();
    qRegisterMetaType<p1::contracts::RoutePlanRequest>();
    qRegisterMetaType<p1::contracts::RoutePlanResult>();
    qRegisterMetaType<p1::contracts::RerouteRequest>();
    qRegisterMetaType<p1::contracts::MapMatchRequest>();
    qRegisterMetaType<p1::contracts::MapMatchResult>();
}
} // namespace p1::contracts
