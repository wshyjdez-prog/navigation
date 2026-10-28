# 导航状态与事件契约（4 号第 1 次作业）

本文件是 `include/contracts/NavigationTypes.h` 与 `include/contracts/NavigationController.h`
的可读说明，供 1 号（装配与页面）、3 号（RoutePlanner / MapMatcher）、5 号（渲染与引导展示）
以及其他要消费导航状态的成员查阅。**本文档描述的是契约，不是已实现的功能**：
第 1 次作业只定义类型、事件、状态转换表与错误码，状态机实现属第 3 次作业。

- 契约头：`include/contracts/NavigationTypes.h`、`include/contracts/NavigationController.h`
- 位置源契约（同批，已先落地）：`include/contracts/LocationTypes.h`、`LocationSource.h`
- 模拟实现：`src/navigation/MockLocationSource.*`、`FixedTrackLoader.*`
- 样例：`tests/fixtures/location/`（见该目录 README.md）
- 状态：`include/contracts/` 由 1 号维护、各模块共同设计；本批由 4 号新增两份文件，未改动 1 号任何文件。

## 0. 范围与不做什么

| 本次做了 | 本次没做（及归属） |
|---|---|
| 导航五态、16 种事件、4 类错误码、引导类型与指令结构 | 状态机实现、偏航判定算法、重算去重实现（第 3 次作业，照本文件转换表编码） |
| `NavigationController` 抽象接口（10 个命令 + 8 个信号） | 真实 GeoClue2 定位源（`src/pal/linux/location/`，第 3 次） |
| `NavigationSnapshot` 聚合视图（13 字段，5 号渲染唯一消费入口） | 路网几何格式定义（属 2/3 号，本契约只引用 `geometryRef` 不透明字符串） |
| 3 号 `RoutePlanner` / `MapMatcher` 的调用形状（只给形状，不替其选算法） | A*/HMM 真实集成、G6 / FFI 联调（X06 未确认，第 4 次） |
| 完整状态转换表 + 已确认的偏航/重算策略（第 ⑥ 节） | 到达半径、长时间无定位阈值、精度过低阈值（仍属第 3 次） |

**Mock 边界**：本次的规划/匹配一律是本地 stub，只用于驱动契约测试与回放演示，
**不得作为真实算法集成、四项 PAL 适配或 G6 联调的验收证据**。

## 1. 统一约定

沿用 `project.md` 第 4 节通用草案与本组约定：

- 距离 **米**、时长 **秒**、速度 **米/秒**、角度 **度**（正北 0，顺时针）、时间戳 **Unix 毫秒 int64_t**。
- 经纬度必须带坐标系标记（`CoordinateSystem`：WGS84 / GCJ02 / BD09），固定样例默认 WGS84，不得省略。
- 全部带 `requestId` / `navigationId`、`dataVersion`、`source`（mock/real）标记。
- **旧任务异步结果不得覆盖当前导航**：以 `requestId` 判定，非当前请求一律丢弃。
- 核心数据结构只用 C++17 标准库（`std::optional` / `std::vector`）；Qt 仅用于 `QString` 与信号槽元类型。
- 只用 Qt 6.4.2 已有 API（无 Qt 6.5+ 新接口）。

## 2. 状态：NavigationStatus（五态）

| 状态 | 含义 | 进入条件 | 离开条件 |
|---|---|---|---|
| `Idle` | 未导航，无有效路线 | 初始态；`stop()` 后；首算失败/规划失败/定位丢失中止后 | `start()` → Planning |
| `Planning` | 已发起路线规划，等 3 号结果 | `start()` 被接受 | 结果采纳 → Navigating；失败/取消 → Idle |
| `Navigating` | 持有有效路线，正在导航 | 首算结果采纳；重算结果采纳（RouteUpdated） | 偏航 → OffRoute；到达 → Arrived；stop → Idle |
| `OffRoute` | 偏航，仍持有旧路线，等待/正在重算 | `offRoute(seq)` 被采纳 | 重算结果采纳 → Navigating；到达 → Arrived；stop → Idle |
| `Arrived` | 已到达终点（**终端态**） | `arrived(seq)` 被采纳 | 仅 `stop()` → Idle；任何异步结果都不能使其复活 |

**关于 Paused（重要设计决定）**：Paused **不是独立状态**，而是 `Navigating` / `OffRoute` 的
**子标志**（`NavigationSnapshot::paused`、`statusChanged(status, paused)` 的第二个参数）。

- 理由一：暂停不改变"当前是否持有有效路线并在导航"这一本质，只是停止推进引导；若独立成状态，
  与 OffRoute/Planning 组合会膨胀到 7+ 个状态，转换表规模翻倍。
- 理由二：暂停期间定位仍在更新、路线仍有效，UI / 5 号渲染读 `snapshot.paused` 即可，不必在枚举里再判一次。
- 理由三：保持五态与作业书一致，5 号渲染与 G6 事件映射的分支数量可控。
- 代价：所有状态判断都要同时看 `(status, paused)`，已在 `NavigationSnapshot` 中集中提供。
  若第 3 次作业需要 Paused 独立成状态，属实现层扩展，须同步修改 `NavigationTypes.h` 注释与本文件转换表。

## 3. 事件：NavigationEvent（16 种）

每个命令或异步结果至少产生一个事件（**被丢弃的也发**），供日志、契约测试与 5 号渲染消费。

| 事件 | 触发时机 |
|---|---|
| `NavigationStarted` | `start()` 被接受：Idle → Planning |
| `RoutePlanningStarted` | 已向 RoutePlanner 发出带 requestId 的规划请求 |
| `RoutePlanApplied` | 首算结果被采纳：Planning → Navigating |
| `RoutePlanRejected` | 首算结果被丢弃（陈旧 requestId 或状态不匹配），状态不变 |
| `NavigationPaused` / `NavigationResumed` | paused 子标志置 true / false |
| `OffRouteDetected` | Navigating → OffRoute |
| `RerouteRequested` | 偏航后发起重算（是否发起由第 ⑥ 节策略决定） |
| `RouteUpdated` | 重算结果被采纳：OffRoute → Navigating（视为已回归） |
| `RerouteRejected` | 重算结果被丢弃 |
| `GuidanceUpdated` | 当前引导指令发生变化 |
| `Arrived` | Navigating / OffRoute → Arrived |
| `NavigationStopped` | 显式 `stop()`：任意态 → Idle |
| `NavigationAborted` | 异常中止 → Idle（无可用路线 / 规划失败 / 定位丢失） |
| `IllegalEventDropped` | 非法命令或事件被丢弃，原因写在 `reason` |

`NavigationEvent` 字段：`kind`、`status`、`paused`、`timestampMs`（墙钟 Unix 毫秒）、
`locationSeq`（可选，与位置有关的事件才有）、`requestId`（0 表示与规划无关）、
`navigationId`、`reason`（被丢弃/失败时必须写明）、`source`。

## 4. 错误码：NavigationErrorCode（四类）

| 错误码 | 含义 | 契约规定的处理 |
|---|---|---|
| `NoRouteAvailable` | 起终点不可达或路网无解 | **首算**：中止导航（无旧路线可继续） |
| `PlanningFailed` | 规划器内部错误 | **重算**：不中止，继续用旧路线导航；**首算**：中止 |
| `PlanningCancelled` | 因 `stop()` / 新 `start()` 主动取消 | 非故障；不提示错误，只发 `NavigationStopped` |
| `AbortedByLocationLost` | 位置源长时间 NoFix | 中止导航（无定位则无法继续导航结论） |

`NavigationError` 字段：`code`、`message`（面向开发者）、`timestampMs`、`requestId`、`navigationId`、`source`。
错误与事件分开：错误是需要 UI 提示或导致中止的失败，事件只是状态变化的记录。

## 5. 引导：GuidanceInstruction

`GuidanceKind` 共 16 种：`Unknown / Depart / ContinueStraight / TurnLeft / TurnRight /
SlightLeft / SlightRight / SharpLeft / SharpRight / UTurn / KeepLeft / KeepRight /
RoundaboutEnter / RoundaboutExit / Merge / ArriveDestination`（未识别填 `Unknown`，不得省略）。

| 字段 | 类型 | 单位 | 可选 | 说明 |
|---|---|---|---|---|
| `kind` | GuidanceKind | — | 否 | 动作类型 |
| `text` | QString | — | 否 | 面向用户的引导文本（语音与 UI 共用） |
| `distanceToManeuverM` | double | 米 | 否 | 当前位置到动作执行点的沿路线距离，≥ 0 |
| `index` | uint32_t | 序号 | 否 | 一次导航内从 0 起单调不重复；重算后沿用新路线自身序号 |

文本由第 3 次作业的引导生成模块填充，本契约只规定结构与序号语义。

## 6. 路线引用与 3 号调用形状

`RoutePlanRef` **只引用不定义**路网几何：`routeId`、`dataVersion`、`geometryRef`（不透明字符串，
格式由 2/3 号定义）、`totalDistanceM`、`totalDurationSec`、`source`、`requestId`。

| 类型 | 方向 | 关键字段 |
|---|---|---|
| `RoutePlanRequest` | 4 → 3 | requestId、issuedAtMs、origin、destination（坐标系须一致）、dataVersion、options（可选，语义由 3 号定义） |
| `RoutePlanResult` | 3 → 4 | requestId、route（optional，失败为空）、instructions（按 index 升序） |
| `RerouteRequest` | 4 → 3 | requestId、issuedAtMs、currentPosition、destination、previousRouteId（可选）、locationSeq（可选） |
| `MapMatchRequest` | 4 → 3 | requestId、position、locationSeq、routeId（可选，空表示无路线） |
| `MapMatchResult` | 3 → 4 | requestId、locationSeq、matched（optional）、distanceToRouteM（optional）、alongRouteDistanceM（optional） |

**匹配结果的语义与阈值由 3 号定义**；本契约只规定可选性与一条硬约束：
未给出判定结果时，消费方**不得默认"在路线上"**（`distanceToRouteM` 为空不等于偏离为 0）。

`RoutePlanner` / `MapMatcher` 接口只提供调用形状（异步 requestId + 信号回流），
第 1 次作业用本地 stub；第 2 次替换为 3 号真实实现时，控制器侧无需改动。

### 6.1 与 3 号模型的坐标边界：度 ↔ 米（2026-10-28 确定）

3 号 P0 模块（`src/algorithms/model/Types.h`，提交 `fdd486e`）的内部表示是**米制平面坐标**
（`CoordinateFrame`：`LocalCartesian` / `ProjectedCartesian`，单位米），其 README 明确要求
"WGS84 经纬度须先由适配层转换，不能将角度直接填为米"。本契约据此确定分工：

| 方向 | 谁负责 | 依据 |
|---|---|---|
| 4 号 → 3 号：`GeoCoordinate`（**度**，带 WGS84/GCJ02/BD09 标记） | **4 号不做任何换算**；度 → 米由 **3 号适配层**（配合 2 号确定坐标框架与投影参数）在 `RoutePlanner` / `MapMatcher` 实现内部完成 | 4 号不掌握数据集的投影参数与局部原点（属 2/3 号的 `CoordinateFrame::definition`）；由 4 号换算等于让导航契约依赖具体数据集，换数据集即失效 |
| 3 号 → 4 号：`distanceToRouteM` / `alongRouteDistanceM` / `totalDistanceM` / `totalDurationSec` | **一律米 / 秒**，由 3 号在其坐标框架内算好后给出，**4 号不二次换算** | 偏航判定（第 8 节）直接消费这些米值，避免两套换算口径导致阈值漂移 |

约束：4 号契约与状态机**不假设任何投影、原点或比例尺**；`RoutePlanRef::geometryRef` 仍是不透明字符串，
其几何的坐标系与单位由 2/3 号定义。若 3 号后续要求调用方直接传米制坐标，属接口变更，
须经 3 号 / 1 号确认后同步修改本文件、`NavigationTypes.h` 文件头第 ⑦ 节与相关测试。

## 7. 状态转换表（完整，第 3 次作业须逐条照此编码）

三类处理策略：**采纳**（改状态 + 发事件）、**丢弃**（不改状态，发 `IllegalEventDropped`
或 `RoutePlanRejected` / `RerouteRejected`，`reason` 写明原因）、**空操作**（仅重复 `stop()`）。

### Idle

| 输入 | 结果 | 处理与理由 |
|---|---|---|
| `start(request)` | → Planning | 采纳：生成 navigationId/requestId，发 NavigationStarted + RoutePlanningStarted + routePlanRequested |
| `locationUpdated(sample)` | → Idle | 丢弃：未导航；定位源可与导航解耦常开，**不计为错误事件** |
| `routeResultReady` / `routeFailed` | → Idle | 丢弃：无在途请求（currentRequestId = 0） |
| `pause()` / `resume()` | → Idle | 丢弃：未导航，谈不上暂停 |
| `arrived()` / `offRoute()` | → Idle | 丢弃：无路线，偏航/到达无意义 |
| `requestReroute()` | → Idle | 丢弃：无当前路线可重算 |
| `stop()` | → Idle | 空操作 |

### Planning

| 输入 | 结果 | 处理与理由 |
|---|---|---|
| `routeResultReady(当前 requestId, route 有效)` | → Navigating | 采纳：RoutePlanApplied，填充 route/剩余距离时间/引导(index=0)，发 GuidanceUpdated |
| `routeResultReady(当前 requestId, route 空)` | → Idle | 中止：错误 NoRouteAvailable + NavigationAborted（首算失败无旧路线可继续） |
| `routeResultReady(非当前 requestId)` | → Planning | 丢弃：RoutePlanRejected，reason = "stale requestId" |
| `routeFailed(当前 id, 非 Cancelled)` | → Idle | 中止：错误 PlanningFailed + NavigationAborted |
| `routeFailed(当前 id, PlanningCancelled)` | → Idle | 采纳取消：NavigationStopped，不算故障 |
| `routeFailed(非当前 id, 任意)` | → Planning | 丢弃：RoutePlanRejected |
| `start(新 request)` | → Planning | 采纳：先 routePlanCancelled(旧 id)，再生成新 requestId 重新规划；navigationId 递增 |
| `locationUpdated(sample)` | → Planning | 采纳但不改状态：记录为最新位置，供规划成功后立即使用 |
| `stop()` | → Idle | 采纳：取消在途请求 + NavigationStopped |
| `pause()` / `resume()` | → Planning | 丢弃：规划未完成不能暂停 |
| `arrived()` / `offRoute()` | → Planning | 丢弃：尚无路线 |
| `requestReroute()` | → Planning | 丢弃：已有在途首算请求 |

### Navigating（paused 可为 true / false）

| 输入 | 结果 | 处理与理由 |
|---|---|---|
| `locationUpdated(sample)` | → Navigating | 采纳：更新 currentPosition；paused=false 时推进引导与剩余距离时间，paused=true 时只更新位置不推进 |
| `pause()` / `resume()` | → Navigating(paused 切换) | 采纳：NavigationPaused / NavigationResumed |
| `offRoute(seq)` | → OffRoute | 采纳：OffRouteDetected，随后按第 8 节策略决定是否 RerouteRequested |
| `arrived(seq)` | → Arrived | 采纳：Arrived |
| `requestReroute(req)` | → Navigating | 采纳：生成新 requestId，发 RerouteRequested + routePlanRequested；旧请求作废 |
| `routeResultReady(当前 requestId)` | → Navigating | 采纳：RouteUpdated（主动重算结果） |
| `routeResultReady(非当前 requestId)` | → Navigating | 丢弃：RoutePlanRejected |
| `routeFailed(当前 id, 任意)` | → Navigating | 采纳错误但**不改状态**：继续用旧路线导航（重算失败不应打断进行中的导航） |
| `routeFailed(非当前 id)` | → Navigating | 丢弃：RerouteRejected |
| `start(新 request)` | → Navigating | 丢弃：必须先 stop()。避免隐式续接导致 navigationId 混乱 |
| `stop()` | → Idle | 采纳：NavigationStopped |

### OffRoute

| 输入 | 结果 | 处理与理由 |
|---|---|---|
| `locationUpdated(sample)` | → OffRoute | 采纳：更新位置；回归判定由第 3 次实现 |
| `offRoute(seq)` 重复 | → OffRoute | 丢弃：IllegalEventDropped，reason = "already off-route"（避免重复触发重算） |
| `routeResultReady(当前 requestId)` | → Navigating | 采纳：RouteUpdated（视为已回归） |
| `routeResultReady(非当前 requestId)` | → OffRoute | 丢弃：RerouteRejected |
| `routeFailed(当前 id)` | → OffRoute | 采纳错误但不改状态：保持偏航提示，等待下次重算 |
| `arrived(seq)` | → Arrived | 采纳：偏航中到达终点同样应结束导航 |
| `pause()` / `resume()` | → OffRoute(paused 切换) | 采纳：暂停不改变偏航状态 |
| `requestReroute(req)` | → OffRoute | 采纳：新 requestId 取代旧请求 |
| `start(新 request)` | → OffRoute | 丢弃：必须先 stop() |
| `stop()` | → Idle | 采纳：NavigationStopped |

### Arrived（终端态）

| 输入 | 结果 | 处理与理由 |
|---|---|---|
| `locationUpdated(sample)` | → Arrived | 采纳但不推进：只更新位置（到达后不再需要引导） |
| `stop()` | → Idle | 采纳：NavigationStopped |
| 其他全部（`start` / `pause` / `resume` / `arrived` / `offRoute` / `routeResultReady` / `routeFailed` / `requestReroute`） | → Arrived | 丢弃：终止态不得被任何异步结果复活；`start` 须先 stop() |

### 跨状态 · 定位丢失

- 长时间 NoFix → **Idle**，中止：错误 `AbortedByLocationLost` + `NavigationAborted`（阈值属第 3 次）。
- 短时 NoFix → 保持当前状态等待恢复，**不作废路线**。

## 8. 已确认的偏航判定与重算去重策略

用户 2026-10-28 从三个候选中选定 **候选 2**（原文："选择2。进行下一步"），
第 3 次作业的状态机实现须遵守本节；样例 `tests/fixtures/location/off_route_recover.csv`
已按此策略构造。记录见 `decision_log.md` 的 `DEC-20261028-4-offroute-reroute-01`。

| 项 | 取值 |
|---|---|
| 偏航判定 | 横向偏离路线 **> 15 米** 且 **持续 ≥ 3 秒**（至少 3 个连续帧）才调用 `offRoute(seq)`；单次超阈值不判定，避免 GNSS 抖动误判 |
| 重算最小间隔 | **10 秒**，距上次重算不足 10 秒不再发起新请求 |
| 重算在途 | 已有在途 requestId 时，新位置**不触发**新规划请求，须等 `routeResultReady()` / `routeFailed()` 回流后再判断 |
| 间隔计算口径 | 按**定位样本的时间戳**（不按墙钟），保证回放测试确定性 |

偏离量本身由 3 号 MapMatcher 给出，本契约只规定阈值与持续时间。
**仍待第 3 次确定**：到达半径、长时间无定位的时长阈值、精度过低阈值。

## 9. 契约硬性规则（实现必须遵守）

### ① 线程归属

- `NavigationController` 是 QObject，所有命令只能在其所属线程调用（正式工程即 GUI 主线程）。
- 信号在控制器所属线程发出。3 号的 RoutePlanner / MapMatcher 若在后台线程计算，
  必须把结果经 `Qt::QueuedConnection` 投递回控制器线程，再调用 `routeResultReady()` / `routeFailed()`。
- 信号参数按值复制，不引用控制器内部存储。

### ② 所有权与生命周期

- 控制器由创建者（UI / 应用装配层）独占持有；消费方只连信号，不持有、不删除。
- 控制器不持有 `LocationSource`；只在其所属线程接收 `PositionSample`。
- 析构等价于先 `stop()`，但不再发出事件；析构后连接由 Qt 自动断开。

### ③ stop() 之后不得再产生回调

- `stop()` 同步执行：取消在途规划请求（`routePlanCancelled`）；若当前不是 Idle，
  发出且仅发出一次 `NavigationStopped` 事件与 `statusChanged(Idle, false)`。
- `stop()` 返回后不再发出 `statusChanged` / `navigationEventOccurred` / `snapshotUpdated` /
  `guidanceUpdated` / `navigationErrorOccurred`，直到下一次 `start()`。重复 `stop()` 为空操作。
- 排队连接的**残留事件仍可能送达**，消费方须比较 `navigationId` / `requestId` 后丢弃。

### ④ 旧异步结果不得覆盖当前导航

- 每次 `start()` 递增 `navigationId`；每次 `start()` / `requestReroute()` 生成新的 `requestId`（> 0，单调递增）。
- 只有 `requestId == currentRequestId()` 的结果才被采纳，其余一律丢弃并发
  `RoutePlanRejected` / `RerouteRejected`，`reason` 写明 "stale requestId"。
- `PlanningCancelled` 只用于主动取消，不计为故障。

## 10. NavigationSnapshot：5 号渲染的唯一消费入口

只读、按值复制的聚合视图，13 个字段：

| 字段 | 类型 | 单位 | 可选 | 说明 |
|---|---|---|---|---|
| `status` | NavigationStatus | — | 否 | 当前状态 |
| `paused` | bool | — | 否 | 暂停子标志，仅 Navigating / OffRoute 有意义 |
| `navigationId` | uint64_t | — | 否 | 导航会话标识，每次 start() 递增 |
| `currentRequestId` | uint64_t | — | 否 | 当前有效的规划请求 ID；0 表示无在途请求 |
| `route` | optional\<RoutePlanRef\> | — | 是 | 当前路线；Idle 时为空 |
| `currentPosition` | optional\<PositionSample\> | — | 是 | 最近一次被采纳的定位样本 |
| `remainingDistanceM` | optional\<double\> | 米 | 是 | 到终点的剩余距离 |
| `remainingDurationSec` | optional\<double\> | 秒 | 是 | 预计剩余时长 |
| `guidance` | optional\<GuidanceInstruction\> | — | 是 | 当前引导指令 |
| `guidanceIndex` | optional\<uint32_t\> | 序号 | 是 | 与 guidance 同步 |
| `dataVersion` | QString | — | 否 | 快照的数据版本（导航侧） |
| `source` | SourceKind | — | 否 | mock / real |
| `updatedAtMs` | int64_t | Unix 毫秒 | 否 | 快照生成时刻 |

可选字段为空时，渲染方**不得**自行假设为 0 或沿用上一次的值，应按"未知"处理（如显示 `--`）。

## 11. 使用方式

### 11.1 编译与链接

```cmake
target_link_libraries(<你的目标> PRIVATE p1_navigation)
```

`p1_navigation`（`src/navigation/CMakeLists.txt`）已 `PUBLIC` 暴露 `include/` 与 `src/`，
并把含 `Q_OBJECT` 的 `LocationSource.h`、`NavigationController.h` 显式纳入 AUTOMOC，
moc 产物编译进该静态库 —— **使用方只需链接 `p1_navigation`，不必自己 moc 契约头**。

### 11.2 元类型注册（跨线程排队连接前调用一次）

```cpp
#include "contracts/NavigationTypes.h"

p1::contracts::registerNavigationMetaTypes();  // 重复调用无害
```

已 `Q_DECLARE_METATYPE` 并注册的类型：NavigationStatus、NavigationEventKind、
NavigationErrorCode、GuidanceKind、GuidanceInstruction、RoutePlanRef、RoutePlanRequest、
RoutePlanResult、RerouteRequest、MapMatchRequest、MapMatchResult、NavigationEvent、
NavigationError、NavigationSnapshot。

### 11.3 消费示例（5 号 / UI 侧）

```cpp
// 控制器由装配层持有，消费方只连信号
connect(controller, &p1::contracts::NavigationController::snapshotUpdated,
        this, [](const p1::contracts::NavigationSnapshot& s) {
            // 状态 + 暂停子标志要一起看
            if (s.status == p1::contracts::NavigationStatus::Navigating && !s.paused) { /* 推进绘制 */ }
            if (s.remainingDistanceM) { /* 用 *s.remainingDistanceM，单位米 */ }
            // 可选字段为空 = 未知，不要当 0
        });
connect(controller, &p1::contracts::NavigationController::navigationErrorOccurred,
        this, [](const p1::contracts::NavigationError& e) {
            // NoRouteAvailable / PlanningFailed / PlanningCancelled / AbortedByLocationLost
            // 按 e.code 分别提示；PlanningCancelled 是主动取消，不应弹错误
        });
```

### 11.4 与 LocationSource 的配合

`MockLocationSource`（第 1 次）与真实 GeoClue2 源（第 3 次）实现同一 `LocationSource` 接口，
`positionUpdated` 直接接到 `NavigationController::locationUpdated()` 即可切换；
切换真实源时导航侧**零改动**。

## 12. Mock 替换点清单（第 2/3/4 次作业）

**Mock 与本地 stub 不作为真实定位、四项 PAL 适配或 G6 联调的验收证据。**
下表是本次留下的全部替换点，"现在用什么"一列的内容都不得计入上述三项验收。

| # | 现在用什么（本次） | 第几次替换 | 替换为 | 替换位置 / 如何替换 | 需要谁配合 |
|---|---|---|---|---|---|
| 1 | `MockLocationSource`（固定轨迹回放，`source=mock`） | 第 3 次 | 真实 GeoClue2 源，实现 `LocationSource` 接口 | `src/pal/linux/location/`（本次留空，只写替换点）；实现 `start/stop/status/kind/sessionId` 与三个信号后，把 `positionUpdated` 接到 `NavigationController::locationUpdated()`。**导航侧零改动** | 4 号主责；1 号提供权限声明/分发配置 |
| 2 | 本地 stub 路线（`navigation_case.json`，`format = p1-stub-route`，`source=mock`） | 第 2 次 | 3 号真实 A* 路线规划 | 实现 `RoutePlanner::requestRoute/cancel` 并回吐 `routeReady/routeFailed`；控制器只认 `requestId`，**契约与控制器侧无需改动**。`RoutePlanRef::geometryRef` 的具体格式由 2/3 号定义 | 3 号主责；2 号定路网几何格式 |
| 3 | 不判定偏航（无匹配实现，偏离量由样例直接给出） | 第 2–3 次 | 3 号 HMM / 最近邻真实 MapMatcher | 实现 `MapMatcher::requestMatch` 并回吐 `matchReady`（`distanceToRouteM`、`alongRouteDistanceM`）；由匹配结果按第 8 节策略（> 15 m 持续 ≥ 3 s）驱动 `offRoute(seq)`。**坐标分工见 6.1 节：4 号传度，米制换算在 3 号适配层内完成，偏离量以米返回** | 3 号主责；4 号消费其偏离量 |
| 4 | 测试内的 `ReferenceController` 脚手架（仅验证契约用，非实现） | 第 3 次 | 4 号正式状态机实现 | 按第 7 节转换表与第 8 节策略编码，落 `src/navigation/`；`NavigationController` 接口不变，契约测试可继续复用 | 4 号主责 |
| 5 | 引导文本由样例/stub 直接填充 | 第 3 次 | 正式引导生成模块 | 产出 `GuidanceInstruction`（kind/text/distanceToManeuverM/index），经 `guidanceUpdated` 发给 1 号语音与 5 号渲染 | 4 号主责；1 号播报、5 号展示 |
| 6 | 无外部核心层，事件只在本地产生 | 第 4 次 | 真实 G6 / FFI 联调（X06） | 需先与 G6 确认事件映射：NavigationStatus 五态 ↔ 核心状态、16 种事件 ↔ 核心事件、4 类错误码 ↔ 核心错误；FFI 的 ABI、回调线程与生命周期由 1、4 号与核心提供方确认。**本次未接入、未联调** | 4 号牵头，全员配合；1 号确认 FFI 边界 |
| 7 | 阈值 15 m / 3 s / 10 s（无真实噪声标定） | 第 3 次 | 用真实定位与真实匹配结果复核 | 如偏差明显，在 `decision_log.md` 新建记录并关联 `DEC-20261028-4-offroute-reroute-01` 重新抉择 | 4 号主责；3 号提供匹配结果 |

## 13. 验证状态（2026-10-28，Ubuntu 24.04.4 + Qt 6.4.2 + GCC 13.3.0）

| 项 | 结果 |
|---|---|
| 两个契约头独立编译单元（`-Wall -Wextra -Wpedantic`） | 退出码 0，warning 0 |
| `cmake -S . -B build -G Ninja` | 退出码 0 |
| `cmake --build build -j2` | 退出码 0，warning 计数 **0** |
| `ctest --test-dir build --output-on-failure` | 退出码 0，**5/5 通过**：ui-contracts、navigation-location、navigation-location-demo、navigation-contracts（19/19）、navigation-location-fixtures（9/9） |
| AUTOMOC | `build/src/navigation/p1_navigation_autogen/.../moc_NavigationController.cpp` 已生成并编入 `libp1_navigation.a` |

**关于契约测试里的 `ReferenceController`**：`NavigationController` 是抽象接口、状态机属第 3 次作业，
为能对"转换表逐条断言 / 陈旧 requestId 丢弃"做可执行验证，
`src/navigation/tests/NavigationContractTests.cpp` 自带一个仅用于测试的参考实现脚手架 ——
它只把本文件第 7 节转换表与第 8 节策略机械翻译成代码，**不含任何几何/匹配/规划算法**
（剩余距离由注入的 provider 提供），**不是**第 3 次作业的状态机实现，也不应被正式代码引用。

**尚未验证**：跨线程排队连接的运行期实测（当前无后台线程场景）；真实 3 号规划/匹配；
真实定位与 G6 联调（本次范围外，第 4 次）。

## 14. 变更与兼容性

- 本次**未改动** 1 号的 `src/app`、`src/ui` 与 `include/contracts/` 下既有文件；`ui-contracts` 仍通过。
- 顶层 `CMakeLists.txt` **无需改动**（`369493b` 已在 `include(CTest)` 之后加入 `add_subdirectory(src/navigation)`）。
- 后续若修改本契约的枚举、字段或转换表，须同步更新：本文件、
  `src/navigation/tests/NavigationContractTests.cpp`、`tests/fixtures/location/` 及其 `expected/`，
  并通知 1 号（装配）、5 号（渲染消费）、3 号（规划/匹配实现）。
