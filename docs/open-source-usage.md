# 5 号第二次与第三次作业开源代码使用说明

日期：2026-11-28（Asia/Shanghai）。

## 第二次作业

| 能力 | 实际实现 | 开源来源 | 5 号是否自行实现算法 |
| --- | --- | --- | --- |
| 路径规划 | 3 号 Qt 适配层调用 Boost.Graph `astar_search` | Boost 1.83，BSL-1.0 | 否，仅消费路线 |
| 地图 HMM 匹配 | 3 号调用 FMM `19ef34e` | FMM Apache-2.0 | 否，仅消费匹配线 |
| 地图绘制 | Qt Widgets/QPainter 的 `drawImage`、`QPainterPath`、`QPen` | Qt 6 开源版 | 5 号适配绘制，不重写图形框架 |
| 异步执行 | Qt `QThreadPool/QFutureWatcher/QImage` | Qt 6 开源版 | 5 号组装任务和所有权 |
| 渲染结构参考 | Navit `graphics.h`、`graphics.c`、Qt5 后端 | Navit GPL v2 | 只参考架构，没有复制源码 |

## 第三次作业

| 能力 | 实际实现 | 开源来源 | 5 号是否自行实现算法 |
| --- | --- | --- | --- |
| 路线来源 | 3 号 Boost.Graph A* | Boost 1.83，BSL-1.0 | 否 |
| 球面距离 | Boost.Geometry `distance` | Boost 1.83，BSL-1.0 | 否 |
| 沿路线位置 | Boost.Geometry `line_interpolate` | Boost 1.83，BSL-1.0 | 否 |
| 航向 | Boost.Geometry `azimuth` | Boost 1.83，BSL-1.0 | 否 |
| 大地测量公式 | geographic Vincenty | Boost 1.83，BSL-1.0 | 否 |
| 仪表盘和回放控件 | Qt Widgets/QTimer/QComboBox | Qt 6 开源版 | 是界面状态组装，不是路径/匹配算法 |

`NavigationSimulation` 仍是界面演示适配器，来源字段固定为 `mock`。它不替代 4 号真实导航状态机；正式接入时应消费 4 号的真实 `NavigationSnapshot`。
