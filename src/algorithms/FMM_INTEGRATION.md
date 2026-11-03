# FMM/HMM 与 Qt MapMatcher 接入

状态（2026-11-01）：**Ubuntu 24.04 独立算法模块 CTest 5/5 通过，失败 0，总耗时 3.51 秒**；真实 FMM/HMM 经新 Qt MapMatcher 适配器运行并生成三条匹配 CSV。已读取共享证据核实，不再仅依据口头反馈。合并前根回归 **4/5、ui-contracts 失败**保留为历史；合入 main `9cc04b1` 后，用户最新确认独立算法 **5/5**、根工程 **7/7**、失败 **0**，libbz2-dev、libosmium2-dev 已补齐；完整导航应用联调未完成。此前 A* Debug/Release 各 3/3 的历史验收保留。

## 来源与调用方式

- [FMM 官方项目](https://github.com/cyang-kth/fmm)，固定提交 [19ef34e1f57ff2f2484231aa0d01dfffea986ec1](https://github.com/cyang-kth/fmm/tree/19ef34e1f57ff2f2484231aa0d01dfffea986ec1)，[官方文档](https://fmm-wiki.github.io/)。顶层 Apache-2.0；第三方子依赖保留原许可，其中 fiboheap 为 LGPL-3.0-or-later。HMM 为上游实现，本组工作是调用、格式转换与 Qt 适配。
- 第三方源码保留在 `~/projects/third-party/fmm-19ef34e1f57ff2f2484231aa0d01dfffea986ec1/`，程序位于 `~/projects/navigation-build/fmm-19ef34e1f57ff2f2484231aa0d01dfffea986ec1/build/bin/`。没有复制第三方源码进本仓库。本轮未改外部 FMM；用户此前已完成 C++17、禁用 Python/SWIG、本地输出目录及 GDAL const 兼容修改，具体补丁继续在外部副本保存。
- 核查固定 ZIP 中的 `CMakeLists.txt`、`src/mm/fmm/fmm_algorithm.hpp`、`fmm_app_config.cpp`、`src/io/mm_writer.cpp`：C++ API 确实存在，可用 `Network`、`NetworkGraph`、`UBODT`、`FastMapMatch::match_traj`。但上游没有提供可直接 `find_package(FMM CONFIG)` 使用的目标，默认不安装头文件；源码 CMake 改全局编译选项、依赖 GDAL/Boost Serialization/OpenMP/H3，并无条件进入 Python。当前选择已独立跑通的 **fmm CLI**，保持导航模块只链接 Qt Core/Concurrent，不把上游全局配置和依赖带入本工程。
- 代价：每个窗口启动进程、重新加载路网/UBODT，适合先完成课程样例闭环；不承诺实时高频定位。后续测到实际瓶颈后，可将同一内部批量接口替换为常驻 C++ 后端。本轮不运行 `stmatch`，不实现最近邻替代。

## 数据流和函数

```text
MapMatchRequest(WGS84, requestId, locationSeq, routeId)
  → FmmMapMatcher: 校验顺序，保留最多 windowSize 个定位点
  → QtConcurrent worker: toNetwork 转为该路网的平面坐标
  → runFmm: 新建本地临时目录，写 id;geom / LINESTRING CSV
  → QProcess 执行真正 fmm，等待完成/取消/超时
  → parseFmmOutput: opath + pgeom + cpath + mgeom + error
  → toWgs84(pgeom.back())
  → 所属线程过滤旧请求，再发 matchReady(MapMatchResult)
```

`runFmm(config, points, cancel)` 是同步批量入口：输入为有序的 **路网坐标**，支持 2..256 点；返回状态、错误消息、逐观测道路 ID、逐观测投影点、完整道路序列、匹配几何、道路误差、数据版本、输入点数、调用耗时及新生成的原始 CSV。供后台线程或独立测试调用，不能直接在 GUI 线程运行。

`FmmMapMatcher::requestMatch(request)` 保持公共签名，必须在对象所属线程调用。requestId 为正数且严格递增；locationSeq 首次允许为 0，之后严格递增；非法经纬度、非 WGS84、倒序或重复序号返回 0，不改变当前任务。有效请求的重复坐标保留。默认窗口 16 点，可设 2..256；首个点异步返回空 `matched`，内部状态 `WaitingForPoints`。routeId 改变清空窗口；调用方在停止、新定位会话或时间断层时调用具体类 `resetWindow()`。公共请求没有时间戳，不能自动识别长时间间隔、计算速度或 GPS 时序质量；只以 locationSeq 排序。

`cancel(id)` 是具体类扩展，取消后静默丢弃回调；`resetWindow()` 取消全部任务并清空窗口/最后结果，允许新会话的 locationSeq 重新从 0 开始，requestId 仍须递增。新请求将旧任务标为取消；worker 每约 50 ms 检查取消并终止子进程（进程启动等待上限 3 秒）。旧结果即使已完成，也不能覆盖新请求。析构取消任务、断开 watcher；后台任务只持有按值复制的配置、坐标及转换函数、共享原子标志，不引用对象。临时目录在子进程回收后删除。

构造时深复制配置（含 ID 映射表）到自有只读共享对象；调用方预先保留的可写表项引用不会修改该副本，任务间共享该配置，每个定位请求不重复复制整个 ID 表。`lastDetails()` 返回 `shared_ptr<const FmmMatchDetails>`，包括 requestId/locationSeq 与批量结果；`detailsReady(quint64)` 在所属线程通知读取。保留句柄可跨越下一次结果替换，模型向量按值复制，Qt 字节串按写时复制管理，无外部可写别名。所有方法与句柄读取在所属线程进行；若另一线程要使用结果，先在所属线程取得拥有所有权的只读句柄。

公共 `MapMatchResult` 只填 requestId、locationSeq、matched。当前位置道路为 `lastDetails()->batch.observationEdges.back()`，不是 `cpath.back()` 的推测。失败/待匹配返回空 matched，内部状态区分原因。**distanceToRouteM/alongRouteDistanceM 保持空**：当前没有传入既定路线几何，FMM 的 error 是到道路距离，offset 是沿单边距离，均不能冒充到既定路线的偏离/进度。

## 坐标、ID 与版本边界

| 项目 | 约定与限制 |
| --- | --- |
| 正式路网 | 由二号提供有向边 Shapefile，属性 `id/source/target`；边方向和反向边由数据明确表达，适配器不自动补边 |
| 坐标 | 生产配置使用 `FmmUnits::Metres`；显式注入 WGS84 度↔该路网米制坐标的双向转换函数，捕获自有、线程安全状态。没有默认投影，没有把角度直接转成 P0 米值 |
| 官方合成样例 | `.prj` 虽标 WGS84，示例算法实际按 0..4 的人工平面坐标运算；测试用 `SyntheticSampleUnits` 与明确命名的标签恒等转换，radius=0.4、gpsError=0.5、UBODT delta=3 均为样例单位，不是米，不可用于真实定位 |
| UBODT | 必须由同一有向路网生成，配置要求非空 networkVersion == ubodtNetworkVersion；下方命令从同一固定样例生成新表并记录输入/表 SHA256。版本标签是供数方的声明，适配器不从无版本字段的 UBODT 反推出来源，不保证错误贴标签的文件兼容。正式导入需二号提供生成来源/校验清单，并冻结同一批文件，运行中不得替换 |
| 道路 ID | `FmmEdgeId` 是有符号 64 位外部 ID，负数可代表反向边；与 P0 无符号 EdgeId、内部数组索引分离。没有默认强转。可显式提供 `p0EdgeIds` 表；提供时所有 cpath/opath ID 都必须有映射，否则 InvalidOutput。未提供时保留独立 FMM ID，不宣称已对应 P0 |
| 输出几何 | pgeom 为逐观测投影点，mgeom 为道路拼接后的匹配线；最后 pgeom 经逆转换填公共 matched。内部完整几何仍为配置指定坐标，不直接交给 WGS84 渲染器 |
| 来源与性能 | 内部带固定 sourceCommit、datasetId、dataVersion、synthetic；公共结果无这些字段，跨模块须连同内部只读句柄交接。elapsedMs 为 runFmm 从输入校验到子进程/解析结束的墙钟时长，包含进程启动和路网加载，不是纯 HMM 内核耗时；早期坐标转换失败不产生有效 HMM 性能样本。没有读取上游 inf 吞吐量作为指标 |

新临时目录中的 `matched.csv` 必须真实存在且可解析，子进程非零、缺文件、格式错误、观测数量不符均失败。不能仅以退出码 0 判定成功，因为固定上游的部分配置失败也返回 0。输入保持原顺序，不静默修复非法坐标。

审核 F1/F2 修复（2026-11-01，已随本次 Ubuntu 独立测试通过）：固定上游无候选返回 `MatchResult{}`，所选五字段的 writer 行为 `0;;;;;LINESTRING()`。仅当六个必需列完整、opath/error/pgeom/cpath 全空且 mgeom 为 `LINESTRING()` 时接受 id=0 为 NoMatch；正常匹配仍只接受 id=1。id=1 无完整路径时也验证其观测字段：全部为空须有合法空 mgeom；保留观测时，道路 ID、有限投影坐标、非负误差和数量必须先验证通过。错误 ID、缺字段、损坏几何或数据不直接降级为 NoMatch。F2 用 optional 表达尚无上一定位序号，首次/reset 后允许 seq=0，后续仍严格递增，requestId 规则不变。回归补入现有 parseResults/windowAndInvalidRequests，原 A*、调度夹具与真实 HMM 测试保持。

## 最小使用示例（装配代码由相应负责人接线）

```cpp
// FmmConfig 配置绝对路径、路网/UBODT版本、坐标定义和米制参数。
// transform 的两项函数由真实数据集投影实现提供，不使用测试恒等函数。
auto* matcher = new p1::algorithms::FmmMapMatcher(config, transform, 16, parent);
QObject::connect(controller, &p1::contracts::NavigationController::mapMatchRequested,
                 matcher, [matcher](const p1::contracts::MapMatchRequest& r) {
                     const auto accepted = matcher->requestMatch(r);
                     // accepted==0 时由调用方记录输入拒绝。
                     (void)accepted;
                 });
QObject::connect(matcher, &p1::contracts::MapMatcher::matchReady, receiver,
                 [](const p1::contracts::MapMatchResult& result) {
                     // 装配方在这里消费结果；空 matched 不默认在路线上。
                     (void)result;
                 });
// stop()/更换位置源/定位断层：在 matcher 所属线程调用 resetWindow()。
```

config、transform、controller、receiver、parent 由装配方提供；本轮没有修改四号控制器或主窗口。完整调用见 `tests/FmmTests.cpp`，经公共 `MapMatcher*` 提交请求并用信号接收。

## 测试与实际验证

| CTest / 用例 | 验证目标 | 当前证据 |
| --- | --- | --- |
| member3-fmm-adapter / parseResults | 有符号道路、pgeom/mgeom、无匹配、空/畸形输出、非有限误差、数量不符 | Ubuntu 已通过（见下方原始日志） |
| invalidBatch | 空/单点、非有限坐标、缺路网、版本不一致、预取消、超时、P0映射成功/缺项、退出0无输出/执行失败 | Ubuntu 已通过；夹具仅验证调度 |
| windowAndInvalidRequests | 少点等待、WGS84/坐标/序号拒绝、重复点、窗口上限、路线/会话切换、只读句柄保留 | Ubuntu 已通过（见下方原始日志） |
| asyncLifecycle | Qt事件循环心跳、后台进程、连续提交、旧结果丢弃、取消、析构、公共信号接收 | Ubuntu 已通过；夹具不验证 HMM |
| failureDelivery | 后台失败经公共接口返回空匹配和内部失败原因 | Ubuntu 已通过（见下方原始日志） |
| member3-fmm-official / officialSample（三条数据行） | 真正 fmm/HMM，固定三条 cpath、几何端点、逐点窗口公共结果、远离路网 NoMatch | Ubuntu 已注册并通过，三条真实 HMM 数据行均通过 |

用户已确认上游独立运行 cpath 为 `2,5,13,14,23`、`25,4,3,5,17,19`、`8,11,13,18,20,24`，本轮真实测试以此为预期，但不会读取上游自带 mr.txt。未提供 P1_FMM_* 三项路径时不注册真实 HMM 测试，并明确输出提示；进程夹具通过不能代替 HMM 验收。本次 tests.json、test-list.log、ctest.log 与 member3-LastTest.log 均核实完整配置已注册并通过 **5 项 CTest**。

Windows 实际检查：Clang 8.0.1 + 本机 Qt 5.14.2 头文件，四份新增 cpp 的 C++17 `-fsyntax-only` 通过，无诊断；测试 moc include 使用空占位，只核查 C++ 前端，**没有验证 moc**。本机 moc 因系统 DLL 重定位错误无法启动；本机缺少团队 Qt 6.4.2，未链接、未运行新测试。两段 Ubuntu 命令的 Bash `-n` 通过（只检查语法，不执行）；Git 差异无空白错误。未将其称为 Ubuntu 验收。根 CMake、公共契约、A* 功能代码和原测试均未修改。

## 本次 Ubuntu 实测证据

本地共享证据目录为 `out/member3-test-evidence/`（Git 忽略）。会话记录指向 `~/projects/navigation-build/member3-fmm-adapter-release/evidence-20261101-145035`。已核对 15 个非空文件，并逐文件 SHA256 校验复制到仓库外 `../navigation-仓库审查资料/member3-acceptance-evidence-20261101-145035/`，附 SHA256SUMS.txt，原证据未修改。

| 验证 | 原始文件 | 实际结果 |
| --- | --- | --- |
| 配置/构建 | evidence-20261101-145035/configure.log、build.log、session.log | 配置生成完成，目标构建至 100%；g++-13 13.3.0、CMake 3.28.3、Qt 6.4.2、Boost 1.83.0。QtTest 横幅中的 GCC 13.2.0 是 Qt 库构建信息，不替代本次 g++ 版本 |
| 五项模块 CTest | evidence-20261101-145035/ctest.log、tests.json、test-list.log、member3-LastTest.log | member3-models、member3-astar、member3-route-planner、member3-fmm-adapter、member3-fmm-official 全部 Passed，5/5，3.51 秒 |
| Qt 程序内部计数 | 同上 | RoutePlanner 11/0；FMM 调度/解析套件 7/0；真实 HMM 套件 5/0。QtTest 总数含初始化/清理；真实 HMM 为三条轨迹数据行，不能把两种套件混为一项 |
| A* demo | evidence-20261101-145035/astar-demo.log | nodes=900 42 5，edges=10 11，cost_m=6，distance_m=6；本次单次 elapsed_ms=0.03672，历史 0.035558 保留为前次测量 |
| UBODT 与样例数据 | ubodt-gen.log、ubodt.txt、dataset-sha256.txt | 本次从固定官方样例 30 边/17 节点路网生成，delta=3 样例单位；UBODT 实物 SHA256 与记录一致，路网四个旁文件/轨迹的五项记录与原固定 ZIP 字节校验一致 |
| 匹配 CSV | evidence-20261101-145035/fmm-results/track-1.csv 至 track-3.csv | 每文件一条 id=1 新输出；六列齐全，逐观测道路/误差/投影点数量一致，坐标有限、误差非负，pgeom/mgeom 末端一致 |
| 合并前根工程回归（历史） | root-regression-LastTest.log | 五项测试中四项 Passed、ui-contracts Failed。已记录 End testing；当前共享证据没有根工程 configure/build/完整 ctest 汇总日志，不能宣称根构建链和回归全部通过 |

| 官方轨迹 | 点数 | cpath | 最后观测道路 | 本次 runFmm 调用耗时 ms |
| --- | --- | --- | --- | --- |
| 1 | 5 | 2,5,13,14,23 | 23 | 37.3338 |
| 2 | 6 | 25,4,3,5,17,19 | 19 | 35.8974 |
| 3 | 6 | 8,11,13,18,20,24 | 24 | 40.0258 |

member3-fmm-official 的 CTest 墙钟为 0.47 秒，内部 QtTest 为 461 ms；表中是各次批量调用（含进程启动和加载），不同统计范围不能混用，均不是稳定性能基准。CSV 的 error 和几何使用合成样例单位，不能当作米制定位质量。

合并前的根失败位于 UiTests::disabledRoutesDoNotChangePage（tests/UiTests.cpp:63，map 页面可进入却预期拒绝）与 pageReplacementTransfersOwnership（:91，页面栈实际 4、预期 3）。由一号核查实际页面行为与测试约定，本轮只交接日志，不修改 UI 或断言，不推断根因。其余 navigation-location、navigation-location-demo、navigation-contracts、navigation-location-fixtures 均通过；这不表示 FMM 已接入导航主窗口。合入 main 后的新回归结果见下方用户反馈。

## Ubuntu 手动构建和运行

以下为复现留档命令，本次已经通过，不要求重跑。在 Ubuntu 新开终端执行以下整段；所有输出在本地目录，不重新下载或解压第三方源码，不修改既有 FMM。

```bash
bash <<'BASH'
set -euo pipefail
pin=19ef34e1f57ff2f2484231aa0d01dfffea986ec1
fmm_src="$HOME/projects/third-party/fmm-$pin"
fmm_build="$HOME/projects/navigation-build/fmm-$pin/build"
data="$fmm_src/example/data"
build="$HOME/projects/navigation-build/member3-fmm-adapter-release"
run="$build/evidence-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$run"
exec > >(tee "$run/session.log") 2>&1
test -x /usr/bin/g++-13
test -x "$fmm_build/bin/fmm"
test -x "$fmm_build/bin/ubodt_gen"
test -f "$data/edges.shp"
test -f "$data/trips.csv"
/usr/bin/g++-13 --version
cmake --version
qmake6 -query QT_VERSION

# 新表取自与测试相同的固定样例，输出为本次 evidence 目录中的新文件。
"$fmm_build/bin/ubodt_gen" \
  --network "$data/edges.shp" --network_id id --source source --target target \
  --delta 3 --output "$run/ubodt.txt" --log_level 2 \
  2>&1 | tee "$run/ubodt-gen.log"
test -s "$run/ubodt.txt"
sha256sum "$data/edges.shp" "$data/edges.shx" "$data/edges.dbf" \
  "$data/edges.prj" "$data/trips.csv" "$run/ubodt.txt" \
  | tee "$run/dataset-sha256.txt"

cmake -S /media/sf_navigation/src/algorithms -B "$build" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/g++-13 \
  -DBUILD_TESTING=ON \
  -DP1_ALGORITHMS_BUILD_ASTAR=ON \
  -DP1_ALGORITHMS_BUILD_QT_ADAPTER=ON \
  -DP1_ALGORITHMS_BUILD_FMM_ADAPTER=ON \
  -DP1_FMM_EXECUTABLE="$fmm_build/bin/fmm" \
  -DP1_FMM_SAMPLE_DIR="$data" \
  -DP1_FMM_SAMPLE_UBODT="$run/ubodt.txt" \
  2>&1 | tee "$run/configure.log"
cmake --build "$build" --parallel 2 2>&1 | tee "$run/build.log"
ctest --test-dir "$build" --no-tests=error -N 2>&1 | tee "$run/test-list.log"
ctest --test-dir "$build" --show-only=json-v1 > "$run/tests.json"
python3 - "$run/tests.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as f:
    names = [test["name"] for test in json.load(f)["tests"]]
expected = {"member3-models", "member3-astar", "member3-route-planner",
            "member3-fmm-adapter", "member3-fmm-official"}
if len(names) != 5 or set(names) != expected:
    raise SystemExit("Unexpected CTest registration: " + repr(names))
print("Confirmed five CTest registrations:", ", ".join(names))
PY
ctest --test-dir "$build" --no-tests=error --output-on-failure -V \
  2>&1 | tee "$run/ctest.log"
"$build/p1_algorithms_astar_demo" 2>&1 | tee "$run/astar-demo.log"
# track-*.csv 为新适配器捕获的真实新输出，不是上游 mr.txt。
cp -a "$build/fmm-results" "$run/fmm-results"
cat "$run"/fmm-results/track-*.csv
printf '\nEvidence: %s\n' "$run"
BASH
```

CTest 中真实样例将输出三条道路序列、点数和本次调用耗时；不要求复现之前 A* 单次计时。若找不到 libFMMLIB.so，先反馈原始日志及 `ldd <上述 fmm 绝对路径>`，不要改系统库或更换 FMM。Debug 复验可换独立 build 目录并把 Release 改 Debug，不复用不同工具链的缓存。

合并 main 前的根回归为 4/5，ui-contracts 失败，当时根 CMake 尚未引入算法子目录。该结果保留为历史验收记录。

合并 `origin/main`（`9cc04b1`）后，当前根 CMake 已通过 `add_subdirectory(src/algorithms)` 纳入算法模块。`P1_ALGORITHMS_BUILD_ASTAR`、`P1_ALGORITHMS_BUILD_QT_ADAPTER`、`P1_ALGORITHMS_BUILD_FMM_ADAPTER` 仍默认 OFF，算法部分默认只构建 P0 模型及启用 BUILD_TESTING 时的模型测试；RoutePlanner/MapMatcher 的主程序装配尚未完成。用户已确认合并后的 Ubuntu 独立算法 CTest 5/5、根工程 CTest 7/7、失败 0，依赖 libbz2-dev、libosmium2-dev 已补齐。本次未提供新的原始日志、运行耗时或构建模式，故不附加这些结论；之前 evidence-20261101-145035 的根 4/5 日志仍是合并前证据。下列命令保留供后续复现，本轮不重跑；根回归不能替代独立算法适配器测试：

```bash
bash <<'BASH'
set -euo pipefail
build="$HOME/projects/navigation-build/root-regression-fmm-integration"
mkdir -p "$build"
log="$build/regression-$(date +%Y%m%d-%H%M%S).log"
exec > >(tee "$log") 2>&1
cmake -S /media/sf_navigation -B "$build" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/g++-13 \
  -DBUILD_TESTING=ON
cmake --build "$build" --parallel 2
QT_QPA_PLATFORM=offscreen ctest --test-dir "$build" --no-tests=error --output-on-failure
printf '\nRoot regression log: %s\n' "$log"
BASH
```

## 交接与阶段边界

- 二号：真实米制有向路网、投影/逆投影、覆盖范围、ID 映射及与 UBODT 同版本的生成记录。适配器不读取二号内部数据库，不假设样例 ID 就是 P0 ID。
- 四号：确认 requestId/locationSeq 生命周期、将停止/定位断层接到 resetWindow、等待/失败的内部状态消费；公共结果缺少道路/版本/来源/取消入口，当前走具体类内部接口，不擅改契约。既定路线距离及偏航判断仍缺真实路线几何供给，不能用道路误差代替。
- 五号：内部几何为网络坐标，须按数据集投影转换后消费，并保留共享只读句柄；正式渲染数据交接尚未进行。
- 一号：通过模块目标 `p1_algorithms_fmm` 装配具体 matcher；根工程注册和 UI 接线本轮未修改。
- 下一步：独立模块及官方合成样例验收已通过，可安排与四五号的样例接口联调；实际接线尚未发生。合并后的根回归已由用户确认 7/7；真实路网投影/UBODT 来源和路线偏离量未就绪前，不宣称已完成完整真实导航联调。未开发页面、FMM 引擎重写、压力框架或其他成员模块。
