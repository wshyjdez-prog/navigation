# 第三位组员 P0：模块私有模型

本目录实现 C++17 图、轨迹、基础校验和只读快照。命名空间为 `p1::algorithms::model`，仅依赖标准库；类型是第三位组员的内部表示。依据《第三位组员技术架构与分阶段开发方案-2026-10-28.md》修订审核版第 9 节实施。

## 文件与入口

| 文件 | 职责 |
| --- | --- |
| `CMakeLists.txt` | 独立构建、模型库、模型测试及 CTest 注册 |
| `model/Types.h` | 强类型 ID/索引、元数据、米制坐标、图、轨迹和只读快照 |
| `model/Validation.h` | 校验错误、校验结果与发布接口 |
| `model/Validation.cpp` | 基础校验及快照发布 |
| `tests/ModelTests.cpp` | 固定输入的确定性模型测试，无第三方测试库 |
| `README.md` | 数据规则、所有权、验证结果及后续条件 |

`validateGraph(const GraphData&)`、`validateTrajectory(const TrajectoryData&)` 返回全部基础校验错误。`publishGraph(const GraphData&)`、`publishTrajectory(const TrajectoryData&)` 返回快照及校验结果；校验失败时快照为空。错误包含 `ErrorCode`、`ObjectKind`、零基输入数组位置、可选几何点位置及字段名。元数据和整体模型错误没有数组位置；位置不是节点/边 ID，也不是轨迹原始序号。错误按元数据、输入对象和字段检查顺序生成，不依赖哈希表迭代顺序。

## 数据语义与校验规则

| 内容 | 规则与边界 |
| --- | --- |
| ID / 索引 | `NodeId`、`EdgeId` 为不同的 64 位强类型；`NodeIndex`、`EdgeIndex` 为不同的 `size_t` 强类型。四者不隐式互转，ID 不作为数组下标。0 是合法 ID；节点 ID 和边 ID 各自唯一，两种 ID 可以数值相同 |
| 数据版本 | `dataset_id`、`version`、`source`、`frame.definition` 必填，空字符串或仅 ASCII 空白报错。版本是保留原文的不透明修订标识，更新内容时由提供者更新版本；P0 不管理全局版本注册 |
| 坐标框架 | 每个图/轨迹只有一个框架，所有点继承该框架。支持 `LocalCartesian` / `ProjectedCartesian`，长度单位必须为 `Metres`；未知枚举拒绝。局部坐标说明原点/轴，投影坐标说明 CRS/转换。P0 检查声明存在，不执行或验证投影变换；WGS84 经纬度须先由适配层转换，不能将角度直接填为米 |
| 数值 | 坐标、边长、代价和可选精度必须有限，拒绝 NaN 与正负无穷。边长/代价/精度负数拒绝；零代价和零精度合法 |
| 图 | 至少一个节点；孤立节点和无边图合法。每条边的 from/to 必须存在；允许平行边和具有非退化几何的自环 |
| 有向边 | 几何按 from→to 排列。反向行驶用另一条边，校验不会补反向边；几何相交/坐标相同不隐含拓扑连接 |
| 边几何 | 至少两点，并有一对相邻点坐标不同。重复点原样保留；全重复几何拒绝。首尾点分别与 from/to 节点比较，欧氏距离 `<= 1e-6 m` 接受，超出报错；不吸附、不反转 |
| 零长度边 | `length_m == 0`（含负零）拒绝，即使几何非退化；全退化几何即使声明正边长也拒绝 |
| 长度与代价 | `length_m` 是输入声明的正长度，`cost_m` 是米制非负距离代价。P0 不求精确折线长度，也不保证代价大于端点直线距离；后续启发式搜索不能仅凭 P0 校验启用欧氏启发 |
| 轨迹 | 至少一点，输入顺序原样保留。`original_index` 严格递增且可跳号；重复或下降报错，不重编号、不排序。位置重复合法 |
| 时间 | `unix_time_ms` 为有符号 Unix 毫秒，支持 epoch 前时间；时间非递减，相等允许并保留全部点，下降报错。时间差为零时的速度/转移处理由后续算法定义 |
| 精度 | `accuracy_m` 可缺省；提供时必须有限非负。它表示设备精度，不自动等于高斯标准差 |

图与轨迹独立校验。后续联合使用时，适配层还须核对数据版本和坐标框架匹配。基础校验不修改输入，不排序、不去重、不夹紧非法数值。分配失败等标准库异常按 C++ 异常传播，不伪装成有效空模型。

## 所有权与发布隔离

`GraphData` / `TrajectoryData` 是可写的输入值，字符串、数组及嵌套几何数组均按值拥有。发布函数只接受 `const &`：即使传入 `std::move(input)`，也先深拷贝全部输入，再校验私有副本，最后将私有副本转移到实际以 `const` 创建的快照中。

快照构造函数私有；内部数据成员为 `const`，只提供 `const &` 访问，发布结果为 `shared_ptr<const ...Snapshot>`。调用方原先持有的节点、几何点、轨迹点或字符串可写引用都只指向原输入，无法修改已发布副本。原输入销毁后快照仍持有全部数据。数据更新需发布新快照；使用者在持有数据引用期间也须保留快照的共享所有权。调用期间不得并发修改输入。

代价是每次发布需要完整复制，峰值内存包含输入和副本；P0 不公开直接接管调用方容器的接口，以免已泄漏的元素引用随容器移动进入快照。

## 确定性测试覆盖

| 规则 | 用例及检查 |
| --- | --- |
| 有效图与元数据 | 稀疏 ID、节点数组逆序、版本/来源/框架保持、孤立节点、0 ID、平行/反向边、自环、投影框架 |
| 唯一性与引用 | 重复节点/边 ID，from/to 分别悬空及同时悬空；核对重复或引用对象的位置和错误顺序 |
| 数值合法性 | 节点、几何、轨迹的 x/y 轴及边长、代价、精度逐类测试 NaN / +∞ / −∞；负长度/代价/精度拒绝 |
| 几何与方向 | 空/单点几何，全重复几何，反向几何，首尾误差超限、二维合成误差超限；重复点保留，首尾容差边界接受且不吸附 |
| 零值 | 零和负零边长拒绝；零代价、零精度接受 |
| 轨迹顺序与时间 | 单点、原始序号跳号、重复/下降序号、下降时间、相同时间、重复位置及 epoch 前时间 |
| 元数据与空输入 | 空图/轨迹，四项缺失声明、逐字段 ASCII 空白，未知坐标系/单位；带周围空白的非空 opaque 版本原样保留 |
| 只读隔离 | 编译期检查 ID/索引类型、快照构造权限及访问返回类型；运行期检查 rvalue 发布后原元素/几何/字符串修改、输入销毁、新版本发布不影响旧快照 |
| 错误与无修复 | 失败用例核对完整错误列表：类别、对象种类、数组位置、几何点位置、字段名；非法输入不能发布，时间/精度不被排序或修复 |

运行期检查使用显式条件和异常报告，最终返回非零表示失败，不依赖 `assert`；`NDEBUG` 不会关闭这些检查。CTest 注册一个测试 `member3-models`，该可执行程序逐项报告上述用例。

## 独立构建与 Ubuntu 验收

目标为 `p1_algorithms_model`（静态库）和 `p1_algorithms_model_tests`。独立入口不调用根 CMake，不查找或链接 Qt；两个目标显式关闭 AUTOMOC/AUTOUIC/AUTORCC。只有作为顶层项目时才调用 `project()` / `include(CTest)`，未来由根工程 `add_subdirectory(src/algorithms)` 接入时，测试开关及顶层 CTest 由根工程管理；本次不改根工程。

在 Ubuntu 24.04 桌面虚拟机的统一工具链下执行，构建目录放来宾本地磁盘：

```bash
NAV_SRC="/实际共享路径/navigation"
P0_BUILD="$HOME/p1-build/member3-p0-debug"

g++-13 --version
cmake --version
ninja --version
cmake -S "$NAV_SRC/src/algorithms" -B "$P0_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++-13 \
  -DBUILD_TESTING=ON
cmake --build "$P0_BUILD" --target p1_algorithms_model_tests -j1
ctest --test-dir "$P0_BUILD" --output-on-failure --no-tests=error

# Release 同样执行，确认 NDEBUG 下运行期检查有效。
P0_RELEASE="$HOME/p1-build/member3-p0-release"
cmake -S "$NAV_SRC/src/algorithms" -B "$P0_RELEASE" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++-13 \
  -DBUILD_TESTING=ON
cmake --build "$P0_RELEASE" --target p1_algorithms_model_tests -j1
ctest --test-dir "$P0_RELEASE" --output-on-failure --no-tests=error
```

预期 CTest 运行 `member3-models`，通过且非零项测试才满足运行验收。执行者记录实际工具版本、命令输出和结果；上述命令不是已执行证据。

本轮补测后建议重跑以上 Debug/Release 命令。原 Qt 工程可使用已配置的构建目录复验，下面也是建议命令，不是对先前实际命令的补录：

```bash
QT_BUILD="/既有Qt工程构建目录"
cmake --build "$QT_BUILD" --target p1-ui-tests -j1
ctest --test-dir "$QT_BUILD" -R '^ui-contracts$' --output-on-failure --no-tests=error
```

## 2026-10-28 Windows 检查记录

- 初始分支 `feature/member3-algorithms`，基线提交 `199aa5f`，工作区干净；实际仓库在 `navigation/`。
- CMake 4.3.0；可运行的 Clang 8.0.1，目标 `x86_64-w64-windows-gnu`，使用已有 MinGW 7.3.0 的 C++ 标准库头文件。
- 模型与测试源文件的 C++17 语法检查通过，`-Wall -Wextra -Wpedantic` 无诊断。命令为：

```powershell
& 'E:\qt\Tools\QtCreator\bin\clang\bin\clang.exe' `
  --target=x86_64-w64-windows-gnu -std=c++17 -Wall -Wextra -Wpedantic `
  -fsyntax-only -I src/algorithms `
  src/algorithms/model/Validation.cpp src/algorithms/tests/ModelTests.cpp
```

- 两个源文件还分别以 `-O0 -g -c` 和 `-O2 -DNDEBUG -c` 编译为 Windows 目标文件，四次均成功且无诊断；产物位于忽略的 `out/member3-p0-objects-Debug/`、`out/member3-p0-objects-Release/`。目标文件编译不包含链接或用例执行。
- 独立 CMake 的 Windows 配置已尝试：旧版 MinGW Make 在中文源码路径上发生编码错误；将相同输入复制到可写 ASCII 目录后，编译器探测在调用 `ar` 时失败。现有 `g++`、`ar`、`ld` 在沙箱内直接启动均报告系统 DLL 重定位错误。
- 初轮 Windows 可执行程序和 CTest 未运行；当时 Ubuntu 运行结果尚未提供。后续用户确认的 Linux 结果及本轮补测状态见下节。

## 2026-10-28 P0 审核与最小收尾状态

- 《第三位组员P0独立代码审核报告-2026-10-28.md》结论为通过，未发现已确认的阻塞性缺陷。本轮仅按 T1–T4 补充测试，模型逻辑和 CMake 与审核快照的 SHA256 一致。
- **原版 P0 验收：用户确认 Linux 独立构建、模型 CTest、原 Qt 工程回归均通过。** 依据为用户本轮确认及审核报告第 2.3、7 节；本轮未登录 Ubuntu 或重新运行这些检查。未附原始日志，实际命令、用例通过数量、构建路径、具体环境版本、Debug/Release 覆盖及与源码指纹的对应关系未核实，不据此补写。
- **收尾补测：已补充，待用户在 Ubuntu 复验。** 先前通过的结果不作为新增用例已运行的证据。

| 审核建议 | 本轮补充 |
| --- | --- |
| T1 | 起点两个分量各为 `0.8 * tolerance` 但欧氏误差超限时拒绝；终点恰等于容差时接受并保留原坐标 |
| T2 | dataset_id/version/source/frame.definition 分别设为 ASCII 空白，核对字段及空元数据位置；图和轨迹的非空 opaque 版本保留周围空白 |
| T3 | 在原非有限值参数循环中补节点 y、几何 x、轨迹 x，各覆盖 NaN、+∞、−∞ |
| T4 | 同一条边 from/to 同时悬空，按顺序核对两个 MissingEndpoint 的位置和字段，禁止发布 |

- 本轮实际检查：修改后的 `ModelTests.cpp` 使用 Windows Clang 8.0.1、C++17、`-Wall -Wextra -Wpedantic`，分别以 `-O0 -g -c` 和 `-O2 -DNDEBUG -c` 编译为目标文件，均成功且无诊断；产物位于忽略的 `out/member3-p0-closeout-Debug/`、`out/member3-p0-closeout-Release/`。本轮未运行链接、模型用例、CTest 或 Qt 回归。

## 遗留风险与 P1 前置条件

- 原版 P0 审核与用户确认的 Linux 验收已通过；完成本轮 T1–T4 补测的 Ubuntu 复验后，P1 仍需另行授权。Debug/Release 的既往覆盖未核实，建议本轮两种模式均重跑。
- P1 补充邻接构建、精确长度、几何投影及固定手算样例；复核坐标尺度与端点容差，确认重复点在长度/投影计算中的处理。
- 真实数据适配时由 2/3/5 号确认坐标转换、单位、ID 映射和版本关系；公共契约及根工程装配由 1 号协调。
- 算法来源的课程允许性、A/B 等算法试验范围仍待确认，P0 未作算法选型。A*、HMM、Qt 异步服务、页面和性能监控按后续阶段授权实施。
