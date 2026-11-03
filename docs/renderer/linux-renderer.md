# LinuxRenderer 无头场景与 A/B 模式

日期：2026-11-01（Asia/Shanghai）。负责人：5 号。状态：代码已实现，Windows Qt 6.12 冒烟测试通过；尚未在统一 Ubuntu 24.04 / Qt 6.4.2 环境复验。

## 目标与输入

第二次作业要求 LinuxRenderer 显示瓦片、路线和标记，并落实：

- `drawTile -> drawImage`
- `drawRoute -> drawLine`
- `present -> flush`
- 渲染和计算不阻塞 UI

当前使用接口方案 I1：由 `MockMapScene.cpp` 生成固定 `MapScene` 和 Mock `NavigationSnapshot`。来源字段明确为 `mock`，不冒充 2 号真实地图、3 号算法结果或 4 号真实定位。后续真实数据由适配层转换为相同 `MapScene`。

## 实现映射

| 作业要求 | 当前代码 | 边界 |
| --- | --- | --- |
| 瓦片 | `MapTile::image` 经 `QPainter::drawImage()` 写入缓存底图 | 瓦片已完成栅格化，加载和坐标系转换由后续适配层负责 |
| 路线 | `RouteLine::points` 经 `QPainterPath` 和 `QPen` 绘制 | 路线几何是显示坐标，不实现 A* 或 HMM |
| 标记 | `MapMarker` 绘制起点、终点和当前位置 | 当前位置在固定场景中使用示例点，不从真实定位推断 |
| 导航展示 | `NavigationSnapshot` 提供状态、剩余距离、时间和引导 | Mock 只验证消费形状；可选字段为空时不伪造数值 |
| `present` | 后台完成任务并发出 `frameReady(QImage, RenderStats)` | 信号回到 GUI 线程后，MapCanvas 更新帧 |
| `flush` | `MapCanvas::paintEvent()` 调用 `QWidget::update()` 触发的实际显示 | 不把后台绘制完成误称为已经显示到屏幕 |

## 正式渲染流程

地图页固定使用缓存底图路径：第一次渲染把全部瓦片合成 `baseCache`。后续若 `dataVersion`、逻辑尺寸、输出尺寸和瓦片集合的缓存键未变化，则直接复用底图，只重绘路线、标记和导航覆盖层。

适合平移、标记或引导变化但底图不变的场景。代价是必须维护缓存键与失效范围，瓦片更新时必须重新生成底图。

`Immediate` 完整重绘代码仅作为开发期正确性对照，不在界面提供切换，也不在普通构建中构建基准程序。用户运行和演示时只有一种地图表现。

## 异步与生命周期

`LinuxRenderer::requestRender()` 只在 GUI 线程接收请求，复制不可变的场景快照并交给 `QThreadPool`。每次请求都有递增 `requestId`：

1. 已运行帧允许完成，但结果不是最新请求时不显示。
2. 运行期间只保留最新待处理请求，避免任务无限排队。
3. 渲染使用 `QImage`，不使用只能在 GUI 线程安全访问的 `QPixmap`。
4. 完成信号回到创建 `LinuxRenderer` 的 GUI 线程，再更新控件。
5. 析构时断开 watcher，并等待线程池已有任务结束。

## 测试

`p1-renderer-tests` 覆盖：

- 场景具有瓦片、路线、标记时能生成非空画面；
- A 模式第二帧命中底图缓存，B 模式不命中；
- A/B 对相同输入生成相同画面；
- 缺少版本、空瓦片、过短路线、非有限坐标被拒绝；
- 导航快照覆盖层改变输出画面；
- 连续请求只发布最新 `requestId` 的帧。

可选的 `p1-renderer-benchmark` 默认关闭。只有显式设置 `P1_BUILD_RENDERER_BENCHMARK=ON` 才会构建，普通用户不需要使用。

2026-11-03补充：本模块已随1号团队整合在统一Ubuntu/Qt6.4.2通过测试及真实Wayland地图首帧与截图检查；原先Windows结果保留为历史。输入仍为Mock，实际证据和剩余事项见 [团队整合验证](../verification/assignment2-team/README.md)。
