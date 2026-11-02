# 2 号「统一数据」现状与对接说明（独立文件）

> 本文件独立描述 2 号在 2026-11-02 交付的 OSMX 统一数据模块，便于小组成员单独阅读与对接；团队正式进度以 `navigation/project.md` 为准，接口字段以 `navigation/docs/osmx-api.md` 为准。

- 负责人：2 号（统一数据：解析、存储、导入页面、供数）
- 分支：`codex/member-2/osmx-unified-data`（基于 `main` `e3ec3c0`，未提交/未推送）
- 状态：**进行中** —— 功能与开发机测试完成；Ubuntu 24.04 / Qt 6.4.2 构建、CTest 与桌面验收**未执行**

## 1. 一句话结论

OSMX 路网已经能被解析成一份**只读、不可变、预建拓扑**的数据集，导入页面成功导入后会发布到进程级 `OsmxStore`；3 号 A*/HMM 与 5 号 Renderer 通过同一个 `OsmxStore::shared().active()` 拿到**同一个实例**，不需要各自解析文件。当前缺的是目标环境（Ubuntu/Qt 6.4.2）的实测证据。

## 2. 交付物与代码入口

| 组件 | CMake 目标 | 源码 | 说明 |
| --- | --- | --- | --- |
| OSMX 解析库 | `p1_data_osmx`（STATIC，仅 Qt 6 Core） | `src/data/osmx/` | 解析、校验、拓扑/属性查询、缓存清单、共享 Store、节点网格 |
| 导入页面模块 | `p1_import_page`（STATIC） | `src/app/`（`ImportPage`/`ImportFactory`/适配器/加载器） | 固定路由 ID `import`，公开工厂 `ImportFactory.h`，仅 GUI 线程创建 |
| 诊断工具 | `p1-osmx-cli` | `src/data/osmx/tools/OsmxCli.cpp` | 打印统计/警告；`--header-only` 只校验头部 |
| 解析库测试 | `data-osmx`（CTest） | `src/data/osmx/tests/OsmxTests.cpp` | 合成路网、9 类损坏文件、进度/取消、缓存、Store、真实文件 |
| 页面级测试 | `import-page-osmx`（CTest） | `src/app/tests/ImportPageTests.cpp` | 真实页面导入 `.osmx` + 模拟重启后缓存重开 |

根 `CMakeLists.txt` 已装配 `src/data/osmx` 与 `src/app`；`MainWindow` 仅把字面量 `"import"` 换成工厂常量，窗口架构未改。

## 3. 什么时候可以对接

- 用户选择 `.osmx` 文件并完成导入，或重启后在导入页点“从本机加载所选数据集”；
- 此时 `OsmxStore::shared().active()` 非空，3/5 号即可读取；
- 应用启动时**不会**自动解析数据集（避免为未使用的数据付出几十 MB 内存与时间），需要页面触发一次。

## 4. 给各模块的对接方式

| 使用方 | 需要的数据 | 调用 | 状态 |
| --- | --- | --- | --- |
| 3 号 A*/HMM | 有向边 + 米制长度、单行、道路分类、最近节点 | `active()` → `outgoingEdgeFirst/outgoingEdgeCount`、`edges()`、`nodeAt()`、`wayAt().attributes`、`nearestNode()` | 接口就绪；开发机验证，Ubuntu 待复验 |
| 5 号 Renderer | 视野内道路、节点几何、桥隧/层/显示名 | 同一 `active()` → `waysInBounds()`、`wayNodePool()`、`nodeAt()`、`displayName()`、`wayAt().attributes` | 同上 |
| 1 号 装配 | 导入页与工厂 | `ImportFactory::pageId()/pageTitle()/pageDescription()/createImportPage()` + `MainWindow::registerPage()` | 就绪；根 CMake 已装配 |
| 4 号（可选） | 位置吸附到路网 | `nearestNode(lat, lon, maxDistanceMeters)`，半径上限约 35 km | 就绪，未联调 |

硬约定：

- 数据只读、不可变，任意线程可并发读，**不需要加锁**；消费者不得修改；
- **不得**直接读 `data.osmx` 原始字节或缓存目录，也不能读标准 OSM 的 SQLite 私有表；
- 单位：WGS84 度、米、km/h；ID 为有符号 64 位 OSM ID；数组索引与 ID 不可混用；
- 双向道路已预生成正反两条有向边，A*/HMM 不需要再判 `oneway`；边权策略、候选边索引由 3 号决定；
- 页面固定 ID `import`，仅 GUI 线程创建；`registerPage` 成功即由窗口堆栈接管所有权，失败不转移。

## 5. 数据与字段要点

- 格式：小端、定长表、UTF-16LE 字符串、无尾随数据（OSMX v1，详情见 `docs/osmx-format.md`）；
- 实体：节点（id/经纬度/标签）、道路（节点引用/标签/bbox/预计算属性）、关系（成员/标签）；
- 道路属性：`RoadClass`、单行/反向单行、桥/隧道、车道、限速、层、显示名、几何长度；
- 有向边：`from/to/way/segment/lengthMeters`，按起点节点分组（CSR）；
- 空间查询：`nearestNode()`（节点网格）、`waysInBounds()`（渲染裁剪，可限定可导航道路）；
- 未提供：时间/交通/转向限制、坐标转换、最近边/候选路段索引（HMM 候选边由 3 号自建）。

## 6. 验证结果

**已做（开发主机，非验收）**：Windows 11 / Qt 6.11.0 / MinGW GCC 13.1.0 / CMake 3.30.5 / Ninja 1.12.1 / Debug。

| 项目 | 结果 |
| --- | --- |
| 等价根装配构建 | 50/50 目标，0 warning（Release 同样 50/50；`BUILD_TESTING=OFF` 38/38、0 测试） |
| `data-osmx` | 18 通过 / 0 失败 / 1 跳过；跳过项为真实文件用例，`P1_OSMX_REAL_FILE` 下单独通过（2.7 s） |
| `import-page-osmx` | 2/2 通过（页面导入 + 重启后缓存重开） |
| 真实 `test.osmx` | 头部校验与完整解析通过；912,730 节点 / 121,964 道路 / 1,658 关系 / 44,986 可导航道路 / 536,688 有向边 / 16,408.873 km；SHA-256 `a9c4d90b…6467`；解析+建索引 2,691 / 2,693 / 2,871 ms（3 次） |

**未做（不得据此声称可运行）**：Ubuntu 24.04 / Qt 6.4.2 构建与 CTest、Wayland 桌面导入、Qt 6.4.2 实际编译、峰值内存（宿主测量失败，标“未测”）、Navit 同源对标（`NAVIT-20261102-2-osmx-import-01`，受阻未测）。

## 7. 复验方式（Ubuntu 24.04）

```sh
src/data/osmx/tools/verify-osmx.sh --file /path/to/test.osmx
```

脚本依次执行：版本核对 → Debug 构建 → `ctest -R 'data-osmx|import-page-osmx'` → 真实文件头部/完整解析（`/usr/bin/time -v`）→ 真实文件单元用例；日志写入 `build-osmx/verify-osmx.log`。通过标准与缺口见 `docs/verification/assignment2-member2/README.md`。

## 8. 未完成 / 待办 / 阻塞

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| Ubuntu/Qt 6.4.2 构建与桌面验收 | 阻塞 | 当前主机无可用虚拟机/WSL/SSH 入口 |
| Qt 6.4.2 编译兼容性 | 未验证 | 代码只用 Qt 6.0+ API，但未在 6.4.2 实际编译 |
| 峰值内存 / 查询基准 | 未测 | 需要目标环境采样 |
| 完整 StorageAdapter | 未开始 | 属第 3 次作业范围 |
| G7/X07 离线包（下载/增量更新/回滚） | 待外部确认 | 本模块只做到本地缓存目录约定 |
| 启动时自动恢复上次数据集 | 未实现（有意） | 改为页面手动重开，避免启动解析开销 |

## 9. 文档与记录入口

| 内容 | 文件 |
| --- | --- |
| 格式与字段语义 | `docs/osmx-format.md` |
| 存储与 `manifest.json` 约定 | `docs/osmx-storage.md` |
| 解析/查询接口契约（3/5 号必读） | `docs/osmx-api.md` |
| 模块说明 | `src/data/osmx/README.md` |
| 验证记录与缺口 | `docs/verification/assignment2-member2/README.md` |
| Navit 受阻报告 | `docs/benchmarks/navit/NAVIT-20261102-2-osmx-import-01.md` |
| 团队正式进度 | `project.md`（§4 对接速查、第 7 节进度与完成记录） |
| AI 使用记录 | `ai_log.md` 对话 14、15 |

## 10. 边界声明

本模块不包含：A* 路径规划、HMM 地图匹配、LinuxRenderer 渲染、算法调用界面、性能监控、主窗口顶层装配、语音导航。上述分别由 3/5/1/4 号负责，本模块只提供数据与导入页面。
