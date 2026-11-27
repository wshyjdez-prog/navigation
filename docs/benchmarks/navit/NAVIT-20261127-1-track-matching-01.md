# NAVIT-20261127-1-track-matching-01

日期：2026-11-27（Asia/Shanghai）。负责人：1（地图页面；匹配算法为 3 号的 FMM 适配，本次未改）。对标状态：**本组已测；Navit 不适用 / 受阻**。

## 范围

地图新增：右键“设为轨迹点”；地图菜单“导出轨迹点…”“进行地图匹配”“清除轨迹点”。匹配复用已有的真实 FMM/HMM 流程（`NavigationWorkflow::runMatch`，CSV 匹配与此共用）。轨迹点超出已加载区域时，先加载其外框 + 1 km 的道路。没有改动匹配算法和参数（UBODT delta 2000 m），所以不触发 AGENTS 第 7 节的方案对比。

## 环境与数据

见 `evidence/environment.txt`：Ubuntu 24.04.5，8 vCPU，9.7 GiB；Release；FMM `cyang-kth/fmm@19ef34e`（`tools/build-fmm-backend.sh` 编译，GDAL 3.8.4、Boost Serialization 1.83）。数据为湖北导入缓存，SHA256 `05b67d42…039025`。

## 方法

`tests/BusinessTests.cpp` 中的 `realTrackMatching`（需要设置 `P1_REAL_MAP_CACHE`、`P1_FMM_EXECUTABLE`、`P1_UBODT_EXECUTABLE`，可选 `P1_TRACK_REPEATS`）：

- inside-1km：加载武昌 30.5530,114.3200 半径 2 km，A* 规划到 30.5590,114.3290（2.58 km）。沿路线每 120 m 取一个轨迹点，加 ±8 m 固定种子扰动（20261126+repeat），共 22 个点，然后进行地图匹配。
- outside-reload：规划到 30.5800,114.3700（4.53 km），取路线后半段，再重新只加载起点 1 km，使轨迹点全部在已加载区域外，共 38 个点。匹配时会先加载轨迹外框 + 1 km 的道路。
- 计时：从调用 `matchTrackPoints()` 到 `completed` 信号。包括建网、写 shapefile、`ubodt_gen`、`fmm`。`algorithm_ms` 只是 fmm 进程的耗时。
- 质量：匹配几何每个顶点到真实 A* 路线的距离（中位 / P95 / 最大），以及匹配长度 / 路线长度。
- 每种情况 3 次；峰值内存由 `/usr/bin/time -v` 测得（含被等待的 fmm/ubodt_gen 子进程）。

## 结果（`evidence/track-raw.log`）

| 场景 | 成功 | 请求耗时 ms（3 次） | fmm ms | 偏离 中位/P95/最大 m | 匹配长度 / 路线长度 |
| --- | --- | --- | --- | --- | --- |
| inside-1km，22 点 | 3/3 | 46,845 / 47,569 / 47,276 | 9,786 / 9,833 / 9,835 | 0 / 0 / ≤13.5 | 0.960–0.969 |
| outside-reload，38 点 | 3/3 | 46,454 / 46,012 / 46,680 | 9,388 / 9,203 / 9,458 | 0 / 0 / ≤6.6 | 0.977–0.980 |

- 整个测试进程（含子进程）峰值 RSS 1,528.5 MiB，总运行 4 分 42 秒。
- 匹配长度略短于路线：第一个轨迹点在路线起点之后，最后一个在终点之前，属正常现象。
- 耗时约 80% 在 `ubodt_gen`：它为整个区域预计算 2000 m 以内的最短路。fmm 自身约 9.5 s，大部分用于读入 UBODT。CSV 匹配也有同样的成本，不是本次引入的。
- 失败路径（自动化测试 `trackPointsExportAndMatch`）：找不到后端时如实报错，保留轨迹点；超出区域时先加载道路，再报后端错误。

## 与 Navit 的比较

| 项 | 结论 | 依据 |
| --- | --- | --- |
| 离线轨迹 HMM 地图匹配 | 不适用 | Navit 0.5.7 没有对一组历史轨迹点做 HMM 匹配的功能；它的 vehicle tracking 是对实时位置逐点吸附，属于在线、单点任务 |
| 实时位置吸附耗时 | 受阻未测 | 本机未安装 Navit；任务也不等价 |
| 导出轨迹点 | 不适用 | Navit 没有“地图上逐点设轨迹并导出 CSV”的对应功能 |

## 改进方向（未实施，需 3 号评审）

- 按路网版本缓存 UBODT：同一区域重复匹配时可省去约 35 s。
- 匹配只用轨迹外框 + 余量内的道路，或按点间最大间距调小 delta。改动 delta 属于算法参数，需要 3 号确认。
