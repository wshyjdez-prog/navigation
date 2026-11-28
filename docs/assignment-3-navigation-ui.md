# 第三次作业：地图导航仪表盘与模拟回放

日期：2026-11-25（Asia/Shanghai）。负责人：P1 Linux 组 5 号。状态：代码与 Windows Qt 编译检查完成；统一 Ubuntu 24.04 / Qt 6.4.2 运行截图待补。

## 本次交付

5 号本次负责地图上的导航展示，不重复实现 4 号导航状态机。演示输入直接使用 `NavigationWorkflow` 中真实 A* 计算得到的路线，按固定速度沿路线生成来源为 `mock` 的 `NavigationSnapshot`，再把快照交给 LinuxRenderer。

沿路线的球面距离、位置插值和航向计算使用 **Boost.Geometry 1.83 的 geographic Vincenty 策略**（Boost Software License 1.0）。代码不再使用手写的“每度米数 + 经纬度线性插值”公式；A* 路线本身仍来自 3 号接入的 Boost.Graph，5 号没有重复实现路径规划。

实现内容：

- 当前位置沿当前 A* 路线移动，并显示带航向的方向标记；
- 转向卡显示前方左转/右转/直行和到达提示；
- 同步显示当前速度、剩余距离、预计剩余时间和“模拟导航”来源；
- 支持开始、暂停、继续、停止，以及 `1× / 2× / 4×` 回放速度；
- 到达终点后显示“已到达目的地”，可重新回放；
- 路线重建或清除时重置模拟会话，不把旧路线位置留在新路线上；
- 地图底图、路线、标记与快照仍然整体复制后交给后台 `QtConcurrent` 绘制，界面线程只更新控件。

## 使用流程

1. 导入本地地图；
2. 在地图上右键设置起点和终点；
3. 点击“计算路线”；
4. 路线出现后，底部显示“模拟导航”控制栏；
5. 点击“开始回放”，观察当前位置、转向、速度和剩余信息变化；
6. 演示中可以暂停/继续、切换 2× 或 4×，也可以停止后重新开始。

模拟导航只用于第三次作业界面演示。来源始终写为 `mock`，不能作为 GeoClue2 真实定位、4 号状态机或 G6 联调的完成证据。

## 接口与线程边界

| 输入/输出 | 约定 |
| --- | --- |
| 路线 | `NavigationWorkflow::state().route`，元素为 WGS84 `{经度, 纬度}`；距离和插值由 Boost.Geometry 计算 |
| 导航快照 | `contracts::NavigationSnapshot` |
| 地图场景 | `renderer::MapScene` |
| 当前位置 | `MarkerKind::CurrentPosition`，航向单位为度，正北为 0、顺时针增加 |
| 渲染入口 | `LinuxRenderer::render(scene, navigation, size, ...)` |
| 数据来源 | 模拟快照强制为 `SourceKind::Mock` |
| 线程 | GUI 线程生成快照；不可变值复制到后台线程完成 QImage 绘制；结果回 GUI 线程显示 |

本实现采用任务书允许的 Qt Widgets / QPainter 路线。没有使用或验证 OpenGL ES，因此不声明 OpenGL 上下文或硬件加速结果。

## 验证

Windows Qt 6.12 / MinGW 13.1 独立检查完成：

- `NavigationSimulation.cpp`、`MapView.cpp`、`LinuxRenderer.cpp` 编译通过；
- 新增的导航模拟测试覆盖推进、暂停/继续、倍速、Boost.Geometry 球面路线长度、到达和非法路线；
- 原有 LinuxRenderer 测试通过；
- 生成离屏预览图，确认路线、起点、终点、当前位置方向标记和转向卡进入同一帧。

统一 Ubuntu 24.04 / Qt 6.4.2 的完整构建、桌面截图和实际鼠标操作需要在项目虚拟机中补做。用户已明确本次不需要 A/B 性能比较，本记录不新增比较入口或性能结论。
