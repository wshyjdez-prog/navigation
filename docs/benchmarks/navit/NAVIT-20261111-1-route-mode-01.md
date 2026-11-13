# NAVIT-20261111-1-route-mode-01

日期：2026-11-11（Asia/Shanghai）。负责人：1（跨模块；路径规划主责 3、导航 4）。关联决策：`DEC-20261111-1-route-mode-01`。状态：**部分完成**。本组两种模式已实测，道路选择规则已与 Navit 源码/配置逐项对照；Navit 规划耗时受阻，未测。

## 范围与对应关系

地图菜单新增“路线模式：一般模式 / 行车模式”。一般模式就是原行为。行车模式参照 Navit 0.5.7 的 car 车辆配置做道路选择：

- Navit 配置：本机 `/home/abt/navit/share/navit/navit.xml` 第 223–239 行（`<vehicleprofile name="car">`），原文存档于 `evidence/navit-0.5.7-car-vehicleprofile.xml`。
- Navit OSM 映射：`navit-gps/navit` 标签 v0.5.7 的 `navit/maptool/osm.c`（由子代理读取 raw.githubusercontent.com；高速路映射与 v0.5.6 一致）。

| OSM highway | Navit 物项类型 | 在 car 配置中 | 本组行车模式 |
| --- | --- | --- | --- |
| motorway / trunk / primary / secondary / tertiary 及 *_link | highway_*、street_n_lanes、street_4/3/2_*、ramp | 是 | 允许 |
| unclassified / residential | street_1_city | 是 | 允许 |
| living_street / service（含 parking_aisle） | living_street / street_service / street_parking_lane | 是 | 允许 |
| track（默认、grade1–3、paved/gravel/unpaved） | track_gravelled / paved / unpaved | 是 | 允许 |
| track（grade4/5、surface=ground/grass） | track_ground / track_grass | 否 | 排除 |
| ferry | ferry | 是 | 允许 |
| pedestrian / footway / path / cycleway / steps / bridleway | street_pedestrian / footway / path / cycleway / steps / bridleway | 否 | 排除 |
| construction / bus_guideway | street_construction / bus_guideway | 否 | 排除 |
| busway / escape / raceway / corridor / platform | street_unkn（无映射行） | 否 | 排除 |

访问限制：`motorcar`、`motor_vehicle`、`vehicle` 取 no/private/agricultural/forestry/delivery/destination 时，去掉车辆通行；specific 的 yes 可以覆盖 `access=no`。这与 Navit 0.5.7 `osm_end_way` 的标志合成式一致。

差异：本组在导入时就把 `access/vehicle/motor_vehicle=no|private` 的道路整体设为不可规划，所以“`access=no` + `motorcar=yes`”在两种模式下都不可用，与 Navit 不同（本次不改导入，记录在案）。Navit 的 `highway=road` 映射为 street_1_city，本组导入时将其视为未知类型、不可规划。Navit 的速度剖面、`route_depth`、through-traffic 限制没有实现，本组 A* 仍按距离计算。

## 本组实测

- 环境：Ubuntu 24.04.5 虚拟机，8 vCPU / 9.7 GiB，Release，GCC 13.3.0，Qt 6.4.2。
- 数据：湖北 PBF（SHA256 `da2b0200…0297ab`）。
- 程序：临时 `RouteModeBench`（存档为 `evidence/RouteModeBench.cpp.txt`，测完已删除）。
- 场景：以武汉中心 114.305,30.593 为圆心、半径 3 km 加载区域，共 2,394 条道路、15,128 个节点、26,673 条有向边。从“两种模式下都可进出”的节点中，以种子 20261111 随机取 40 对起终点；每对分别用全图与行车子图运行同一个 Boost.Graph A*。

| 指标 | 一般模式 | 行车模式 | 说明 |
| --- | --- | --- | --- |
| 可规划的有向边 | 26,673 | 15,935 | 行车模式去掉 40.3% |
| 构建行车子图 / ms | — | 中位 1.06，最大 1.55（n=10） | 每次规划都会构建 |
| A* 耗时 / ms（n=35） | 中位 3.49，P95 3.83 | 中位 2.43，P95 2.66 | 两种模式都成功的 35 对 |
| 成功 / 不可达 | 35 / 5 | 35 / 5 | 不可达的为同一批起终点（单行/断头） |
| 经过非机动车道路的路线 | 19 / 35，中位 180 m | 0（按构造） | 一般模式会借用人行道等 |
| 距离比 行车/一般 | — | 中位 1.001，最大 1.352 | |
| 峰值 RSS / MiB | 30.7（整个基准进程） | 同左 | |

## 规划区域自动扩大（同日修复）

问题：标准 OSM 规划只加载“起终点外框 + 1 km”内的道路，路线需要绕出这个范围时（绕湖、走远处的桥、单行系统）报“无法规划：no directed path”。修复：A* 返回不可达时，区域按起终点外 2 → 4 → 8 km 扩大后重试，直到超过 `buildNetwork` 的规模上限（25 万节点 / 50 万边）；A* 与吸附规则不变。

复测：用户本机湖北缓存（与报告同源 SHA256），与地图相同的流程（以起点为中心加载 1 km 半径后规划）。程序 `evidence/PlanRetry.cpp.txt`（临时，已删除），原始数据 `evidence/plan-area-expansion.csv`。

- 之前失败的案例（终点康魅路停车场，起点正西 7.5 km）：扩大到 2 km 后成功，11.8 km，76–81 ms。外扩 1 km 时区域网 2,459 节点，起点只能到达其中 216 个；外扩 2/3/5 km 均成功，路线最北到纬度 30.4490，比起终点所在纬度高约 2.3 km。
- 武汉城区 40 对随机起终点（3–10 km，种子 20261111），两种模式：

| 指标 | 一般模式 | 行车模式 |
| --- | --- | --- |
| 成功 / 40 | 34 | 35 |
| 其中靠扩大才成功（原先会报“无法规划”） | 6（2 km×5，8 km×1） | 6（2 km×5，8 km×1） |
| 仍报“无法规划” | 0 | 0 |
| 其他失败（随机点 500 m 内无路 / 吸附点无出边） | 5 + 1 | 5 |
| 无需扩大的规划耗时 中位 / P95 ms | 50.6 / 80.3 | 50.1 / 86.6 |
| 扩大后的规划耗时 中位 / P95 ms | 116.1 / 205.4（n=6） | 115.9 / 215.7（n=6） |
| 进程峰值 RSS / MiB | 50.9（整个检查程序） | 同左 |

Navit 不按区域截取路网（它在整张 binfile 上按 `route_depth` 分级搜索），所以没有对应的“扩大区域”步骤，此项记为不适用；同起终点的规划耗时仍受阻未测。

## 与 Navit 的比较

| 项 | 结论 | 依据 |
| --- | --- | --- |
| 道路选择规则 | 已对照（见上表） | Navit 0.5.7 配置与 maptool 源码 |
| 同起终点规划耗时 / 路线 | 受阻未测 | 本机 Navit 0.5.7 只有 GTK/internal GUI，没有可脚本化的规划与计时入口，本轮未建立 dbus 绑定或源码计时 |

补测方法：从源码构建 Navit，启用 dbus 绑定，或在 `route_graph_build` / `route_path_new` 前后加计时，用同一湖北 binfile（maptool 19.0 s 已生成）在同 40 对起终点上比较耗时与路线长度。负责人：1，需与 3、4 号协作。
