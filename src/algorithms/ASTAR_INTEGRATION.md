# Boost.Graph A* 接入：阶段 A

日期：2026-11-01。分支 `feature/member3-open-source-integration`，起始提交 `ca12fe6`，最初开发工作区干净，后续保留未提交修改。用户已确认 Ubuntu 24.04 的 Debug、Release 均 CTest 3/3 及 demo 正常；最新原始根回归日志为 4/5 通过，ui-contracts 失败（见下方），完整应用联调未完成。保留 A* 实现与公共接口；FMM/HMM 外部引擎及 Qt 适配已通过独立样例验收，正式接线与根 UI 失败仍待相应负责人处理。

## 最新 Linux 验收记录

| 项目 | 实际状态与证据来源 |
| --- | --- |
| Debug 编译、链接 | 用户本轮明确确认在 Ubuntu 24.04 成功；本助手未代执行 |
| Debug Boost | 用户确认 1.83.0 |
| Debug CTest | 用户确认 3/3 通过；未提供各程序内部通过数量，不补写 |
| Debug demo | 用户确认节点 `900→42→5`、边 `10→11`、`cost_m=6`、`distance_m=6` |
| Debug demo 单次耗时 | 用户提供约 `0.080893 ms`；不是平均值、P95 或性能达标证据 |
| Release CTest | 用户本轮确认 3/3 通过 |
| Release demo | 用户确认节点 `900 42 5`、边 `10 11`、`cost_m=6`、`distance_m=6`、`elapsed_ms=0.035558` |
| 本次组合模块验收 | 原始 ctest.log 与 member3-LastTest.log 确认 5/5 通过，含 P0/A*/RoutePlanner/FMM 适配及真实 HMM，3.51 秒 |
| 本次 demo | 原始 astar-demo.log：节点 900 42 5、边 10 11、代价/距离均 6，单次 0.03672 ms |
| 本轮根工程回归 | root-regression-LastTest.log：4/5 通过，ui-contracts 两个断言失败，交一号核查 |

历史和本次单次 demo 耗时分别记录，不据此计算平均值或作性能达标比较。新共享证据位于 out/member3-test-evidence/evidence-20261101-145035，session.log 显示 g++-13 13.3.0、CMake 3.28.3、Qt 6.4.2，configure.log 显示 Boost 1.83.0。根回归的 ui-contracts 在 UiTests.cpp:63 与 :91 失败（map 页面拒绝预期、页面栈 4/3）；没有修改其他成员实现，也不据此推断原因。根工程构建日志尚未随此证据提供。详见 FMM_INTEGRATION.md 的实测记录。

## 文件清单（仓库相对路径）

| 新增/修改 | 文件与作用 |
| --- | --- |
| 新增 | `src/algorithms/core/BoostAStar.h`、`BoostAStar.cpp`：Boost 图转换、搜索、恢复和结果 |
| 新增 | `src/algorithms/samples/FixedRoadNetwork.h`：固定米制有向图 |
| 新增 | `src/algorithms/demo/AStarDemo.cpp`：节点/边、距离/代价和耗时输出 |
| 新增 | `src/algorithms/tests/AStarTests.cpp`：核心边界与独立对照测试 |
| 新增 | `src/algorithms/qt/BoostRoutePlanner.h`、`BoostRoutePlanner.cpp`：后台公共契约适配 |
| 新增 | `src/algorithms/tests/RoutePlannerTests.cpp`：异步、取消和生命周期测试 |
| 新增 | `src/algorithms/tests/TestSupport.h`：P0/A* 共用的简单计数与失败报告辅助代码 |
| 新增 | `src/algorithms/ASTAR_INTEGRATION.md`：来源、边界、验证及交接说明 |
| 修改 | `src/algorithms/CMakeLists.txt`、`README.md`：可选目标、测试注册和说明入口 |
| 修改 | `project.md`、`decision_log.md`、`ai_log.md`：3 号进度、单方案选择和真实工作记录 |

P0 模型、根 CMake、公共契约和其他组员功能代码保持不变。P0 测试仅提取重复辅助代码，所有测试输入与断言保留。

## 开源来源与依赖

| 项目 | 版本、许可证及接入方式 |
| --- | --- |
| [Boost.Graph](https://github.com/boostorg/graph) | 调用 `boost::astar_search`，不复制或修改上游源码。使用系统 Boost 头文件，CMake 最低允许 1.74；Ubuntu 24.04 默认 [`libboost-dev`](https://packages.ubuntu.com/noble/libboost-dev) 指向 1.83。实际来宾版本须以 CMake 输出及 `dpkg-query` 为准。本机 Windows 检查使用 Anaconda 已安装的 Boost 1.82 头文件 |
| [Boost Software License 1.0](https://www.boost.org/LICENSE_1_0.txt) | 上游 `astar_search.hpp` 标明此许可。调用与类型适配由本组编写，A* 实现来自 Boost，不声明算法原创；当前不分发上游源码。后续随源码分发第三方内容时保留其版权和许可 |
| [FMM](https://github.com/cyang-kth/fmm) | 固定 `19ef34e1f57ff2f2484231aa0d01dfffea986ec1`，外部 C++ 引擎、UBODT、真实 HMM 与 Qt MapMatcher 独立样例测试均已通过。顶层 Apache-2.0，保留子依赖许可；上游兼容修改仅在外部副本，导航工程通过后台 CLI 调用，不直接 add_subdirectory 上游。实际日志与坐标/接口限制见 [FMM 接入说明](FMM_INTEGRATION.md) |

Boost 的本次用法是头文件模板算法，不需要链接 `libboost_graph`；同时需要完整 Boost 头文件依赖，单独复制 graph 仓库不足以构建。Qt 适配器额外依赖统一版本 Qt 6.4.2 的 Core、Concurrent，测试依赖 Test。保留原根 CMake，当前阶段通过 `src/algorithms` 独立入口验证。

## 模型如何转换及搜索

`findRoute(graph, originNodeId, destinationNodeId, cancellation)` 是同步纯 C++ 核心，只能在工作线程或命令行测试中调用。输入为 P0 `GraphData`、两个强类型节点 ID、可选共享取消标记。输出 `RoutePath`：状态/原因、节点和边 ID 序列、原米制几何、总代价、总长度、数据集/版本与实际单调时钟耗时。

1. 保留 P0 校验，非法图返回 `InvalidGraph` 及原始校验错误；不存在的节点返回 `InvalidNode`。
2. 将节点 ID 映射为 Boost 紧凑顶点编号，按输入顺序向 `adjacency_list<vecS,vecS,directedS>` 加边。每条边携带原数组位置及 `cost_m`，不自动补反向边。
3. 调用真实 Boost `astar_search`。使用 `h=0`，因为 P0 允许零代价且未证明欧氏距离为代价下界。它是零启发的 A*，搜索行为退化为 Dijkstra；当前不宣称具有几何启发加速。
4. Visitor 在改善最优代价时保存具体前驱边，目标从优先队列取出时结束。回溯具体边、反转结果后累计 `length_m`，所以平行边不会被仅凭节点序列误恢复。
5. `total_cost_m` 是最小化目标，`total_distance_m` 是选中道路声明长度之和，两者分别返回。长度尚未由几何重新计算。几何原样拼接，保留重复接缝和 P0 容差范围内的微小端点差，不吸附或补道路。

边代价有限非负；边长有限正数。A* 另有明确的道路数值域：单边 `length_m` / `cost_m` 分别不超过 `1e9 m`，全图各自累加不超过 `1e12 m`，边界包含等号。`BoostAStar.h` 的两个常量定义此域，入口逐边同时检查单值和总量，超范围返回带边数组位置和约束说明的 `NumericError`。P0 仍接受一般有限值，不代表所有 P0 模型都属于本路由器支持的道路数据。

单边上限为一百万公里，全图总量上限为十亿公里，留有普通道路数据的空间，并使允许输入的简单路径累加、搜索候选扩展远离 double 溢出；总量检查采用先比较剩余额度再相加，搜索和恢复使用普通 double 加法。不缩放、不截断权重，不再因溢出候选支路抛错中止搜索。审核报告中 `1e308` 级反例明确属于不支持的输入，测试检查其被预先拒绝，也检查范围内高代价死支路不妨碍有限目标路线。

起终点相同是成功的单节点路径，距离/代价为 0；不可达返回 `Unreachable`，不返回成功空路径。取消、数值错误和异常均不暴露部分路线。

`elapsed_ms` 包含 P0 校验、Boost 图转换、搜索与恢复，不含 Qt 排队和投影时间。它是一次真实调用的测量值，不是基准统计。取消检查位于图转换、Boost 访问节点/边和路径恢复中；P0 整体校验本身尚不可中断。

## RoutePlanner 适配边界

`BoostRoutePlanner` 继承现有 `p1::contracts::RoutePlanner`，不修改公共头文件。构造输入是不可变 P0 图快照、`CoordinateProjector` 和明确的参考速度（米/秒）。

- `CoordinateProjector` 必须把带 WGS84 标记的经纬度转换为该快照的米制框架；由 2/3 号根据真实数据集投影参数提供。它在后台调用，须按值拥有线程安全资源，不捕获 QWidget 或无保护的数据库连接。未提供投影器、未知坐标系或版本不一致时拒绝请求。
- 首版只支持已有拓扑节点作为起终点。投影位置须在 `1e-6 m` 内唯一对应一个节点，否则异步失败；不做任意道路内插点，不把最近节点猜成输入端点。测试中的 `fixtureProjector` 仅把两个固定 WGS84 标签映射到合成节点，是测试设施，不是通用投影实现。
- `requestRoute()`、`cancel()`、只读访问器均在对象所属线程使用。输入非法、ID 为 0/不递增、版本不匹配、非空 options 不受支持或提交失败时返回 0。合法输入保留调用方 requestId；一个实例代表一个消费通道，新请求取消该实例旧请求。
- 核心与投影均由 `QtConcurrent::run` 在后台执行。`QFutureWatcher` 完成后回到所属线程检查取消标记及最新 requestId，再发 `routeReady` / `routeFailed`。不在 GUI 线程同步等待结果。
- `cancel(id)` 后不再回吐该 ID 的成功或失败，遵守公共契约的静默取消规则。工作任务按值持有快照、投影器和取消标记，不捕获规划器；对象析构取消任务，Qt 自动断开回调。注入的投影器必须自行保证有界执行，不能由核心强制中断其外部阻塞操作。
- `RoutePlanResult` 返回原 requestId；`RoutePlanRef` 填数据版本、真实算法来源、米制距离及几何引用。`totalDurationSec` 按调用方明确提供的恒定参考速度估算，不能解释为真实交通 ETA；不生成未经实现的转向指令。
- 节点、边、几何和核心耗时可通过 `lastPath()` 读取。新请求只改变在途任务，不清理已交付路线；失败/取消保留旧 `geometryRef()` 和数据。成功后最新视图切换到新路线，但旧引用仍可通过 `pathForGeometry(oldRef)` 获取不可变共享所有权，直到接收方确认交接后调用 `releaseGeometry(oldRef)`。保留句柄的使用者可在释放引用后继续读取旧数据；需要保存时复制句柄，勿长期借用 `lastPath()` 返回的内部引用。
- 几何保存用普通 map/shared_ptr 实现，不是全局或持久化仓库；应用接收方负责在接受替换路线、停止使用路线时释放引用，否则规划器将一直保留已交付数据直到析构。释放当前引用会清空最新视图，不取消在途计算。这些方法仅新增于 3 号具体类，未修改公共 RoutePlanner 签名。5 号正式渲染仍需与 2/3 号确认反向投影，不能直接把米制几何当 WGS84 绘制。

现有 `NavigationController.h` 的 QObject 元对象代码由 `p1_navigation` 持有。独立构建临时用 `p1_algorithms_contracts` 生成相同公共契约的 moc；未来父工程已有 `p1_navigation` 时直接复用。不要同时把独立生成的契约库与另一个重复 moc 库打包进同一应用。本轮未改主窗口、应用装配、根 CMake、导航或渲染模块，尚未做跨组真实联调。

## 测试与实际检查

| 测试 | 覆盖 |
| --- | --- |
| `member3-models` | 原有 P0 用例完整保留，共用简单测试辅助代码 |
| `member3-astar` | 正常路径、平行边、空图、非法起终点、零代价/长度分离、取消、非法权重/坐标和数值域；固定图全部节点对以测试专用 Floyd–Warshall 作对照，合并孤立节点、单行反向、同节点的原有断言 |
| `member3-route-planner` | 契约结果/版本/来源、新旧成功路线交接与释放、首次不可达、旧路线后失败/取消重算、非法请求/缺图、投影失败及异常、取消后无回调、连续提交丢弃旧结果、析构和事件循环响应 |

示例可执行程序保留，在 Ubuntu 命令中单独运行；不再把相同固定样例额外注册为 CTest。预期节点 `900→42→5`，边 `10→11`，代价和距离均 6 米。

## 2026-11-01 独立审核后的修复与精简

依据《第三位组员Boost.Graph-AStar独立审核报告-2026-11-01.md》，本轮修复 F1 旧路线生命周期，并按上文数值域闭合 F2；审核报告原文件未改。审核时的 V1 后续已取得用户确认的 Debug/Release 各 3/3 及示例成功结果，A* 独立运行已验证；根工程回归和完整导航装配不由此推定完成。

| 统计口径（源码静态计数，非实际运行通过数） | 精简前 | 修复后 |
| --- | ---: | ---: |
| P0 展开参数循环的命名用例 | 71 | 71 |
| A* 命名用例 | 11 | 8 |
| Qt 业务测试槽（不含 `_data`、init/cleanup） | 7 | 7 |
| Qt 业务执行场景（展开数据行，不含 init/cleanup） | 7 | 9 |
| 三个测试程序业务用例合计 | 89 | 88 |
| 两个模块开关开启时的 CTest 项 | 4 | 3 |

去重明细：

- P0/A* 共同提取 `require`、通过/失败统计和报告到 `tests/TestSupport.h`，保留失败返回非零与所有 P0 断言，不引入测试框架。
- A* 删除单独的 `unreachable isolated node`、`one-way road must not gain reverse traversal`、`same node has zero-length successful path` 名称；已有全节点对对照覆盖相同输入，并补入原来失败空结果和同节点路径形状断言。独有覆盖未删除。
- Qt 的 `unreachableReturnsFailure` 合并进 `replanningPreservesDeliveredRoute` 三行数据：首次不可达、旧成功后失败、旧成功后取消；原错误码、请求 ID 和无成功结果断言保留。成功测试同时验证排队交接时旧引用仍可解析、交接后显式释放及外部句柄继续持有。
- 取消/响应性、旧结果丢弃、析构、投影异常测试保留，它们不与路径正确性测试重复。
- 移除 `member3-astar-demo` 的重复 CTest 注册，保留 demo 构建目标及下面的显式执行命令。

本轮实际检查：Windows Clang 8.0.1 + Boost 1.82，P0 校验、P0 测试、A* 核心、A* 测试、demo 五个源文件分别通过 Debug/Release 目标文件编译（10 次），`-Wall -Wextra -Wpedantic` 无诊断，产物在忽略的 `out/member3-astar-review-fix-Debug/`、Release 同名目录。未运行链接、测试用例、Qt 6.4.2 编译或 Ubuntu 验证。

下面保留首次开发时的历史检查，不能代替本轮运行证据。

2026-11-01 本机实际检查：Windows Clang 8.0.1 + Boost 1.82 头文件，C++17、`-Wall -Wextra -Wpedantic`；核心、核心测试、命令行示例分别通过语法检查和 Debug `-O0 -g` / Release `-O2 -DNDEBUG` 目标文件编译（共 6 次目标文件编译），无诊断。产物在忽略的 `out/member3-astar-Debug/`、`out/member3-astar-Release/`。

Windows 当前未发现 Qt 6.4.2 开发包，仅有旧 Qt 5.14.2，未用它代替统一版本验证。本轮未运行链接、A* 用例、CTest、Qt 适配器编译或运行；不提供虚构的用例通过计数、耗时、内存或 Ubuntu 结论。Qt 适配器和构建配置均待下面的 Linux 检查。

## 当前仅需补充的根工程回归

Debug/Release 算法模块已通过，不重复开发或要求重新运行。根工程仍须单独验证，不能由算法三个 CTest 推定通过。用户在 Ubuntu 执行：

```bash
set -euo pipefail
OUT="$HOME/projects/navigation-build/member3-root-regression-Release"
mkdir -p "$OUT"
cmake -S /media/sf_navigation -B "$OUT/build" -G Ninja \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++-13 -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON 2>&1 | tee "$OUT/configure.log"
cmake --build "$OUT/build" -j1 2>&1 | tee "$OUT/build.log"
ctest --test-dir "$OUT/build" --output-on-failure --no-tests=error \
  --output-log "$OUT/ctest.log"
```

已收到本次根回归 LastTest.log，ui-contracts 失败；以上命令仅供一号处理后复验，不要求本轮重跑已通过的算法测试。完整根构建与 ctest 汇总日志可在后续复验时留存。

AI 阶段与审核报告统一在仓库外：参见 [阶段报告](../../../navigation-仓库审查资料/STAGE_REPORT_2026-11-01.md)、[FMM 准备审核](../../../navigation-仓库审查资料/member3_fmm_preparation_review.md)。本轮源代码准备命令见仓库外的 [FMM 执行说明](../../../navigation-仓库审查资料/FMM_CPP_PREPARE_2026-11-01.md)。不新增仓库内阶段报告。

## 交接及个人汇报素材

- 2 号：提供 P0 不可变图快照、节点/边 ID、版本、方向和米制权重；明确真实投影定义并交付可在后台调用的 WGS84→米制转换。当前没有从数据模块读取数据库或假称完成导入对接。
- 4 号：使用现有 requestId、routeReady/routeFailed/cancel 形状；在控制器所属线程接收结果，并保留其自身的陈旧 requestId 过滤。本轮不实现导航状态机。
- 5 号：当前可消费文本节点/边与米制几何，正式地图仍需约定 geometryRef 解析、生命周期及反向投影。本轮不接手 LinuxRenderer。
- 汇报可用事实：复用 Boost.Graph 的 A*，完成 P0 强类型图向有向 Boost 图的适配、前驱边恢复及距离/代价统计；编写独立对照测试与不改公共契约的后台规划适配器。算法原作者属于 Boost，个人工作是适配、工程封装和验证设计。用户已确认 Ubuntu Debug/Release 各 3 项 CTest 和示例通过；根回归已执行但 UI 项失败，完整导航联调尚未完成；不将单次耗时扩展为性能统计。
