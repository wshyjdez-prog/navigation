# LinuxRenderer 与 Navit 对标实现

负责人：5 号。当前范围是第二次作业的地图绘制、异步提交和性能对标准备。

## 参考来源与许可边界

实现参考 Navit `trunk` 的公开图形接口和 Qt 后端结构：

- `navit/graphics.h`：`graphics_methods` 的统一后端接口，覆盖线条、多边形、图片、文字和绘制周期。
- `navit/graphics.c`：显示列表、图层顺序、可见区域裁剪、绘制开始/结束和异步加载。
- `navit/graphics/qt5/graphics_qt5.cpp`：基于 `QPixmap` 和 `QPainter` 的 Qt 后端。
- `navit/graphics/qt5/QNavitWidget.cpp`：把缓存位图和覆盖层呈现到 QWidget。

Navit 仓库标识说明为 GPL v2，上述文件也带有 GPL v2 头。本项目没有复制 Navit 源码，只借鉴“后端接口、缓存底图、图层顺序、开始/结束提交”的公开架构，并使用 C++17/Qt 6.4.2 独立实现。若后续直接引入 Navit 代码，必须先确认整个项目的许可证兼容性并记录版本和修改。

## 当前实现

- `MapScene.h`：瓦片、路线、标记和逻辑画布尺寸的模块内输入契约。
- `LinuxRenderer`：异步渲染入口；所有请求带递增 `requestId`，只交付最新请求的帧。
- 正式页面固定使用 `CachedBase`：缓存由瓦片组成的底图；路线、标记和导航覆盖层每次重绘。
- `Immediate` 仅保留为开发期正确性对照，不显示在用户界面，也不参与正常运行流程。
- `drawBase()`：把瓦片通过 `QPainter::drawImage()` 写入底图。
- `drawOverlays()`：把路线通过 `QPainterPath` / `QPen` 绘制，再绘制标记与 `NavigationSnapshot` 覆盖层。
- `MapPage`：提供模式切换、异步首帧、尺寸变化重绘和渲染状态显示。

当前 `MapScene` 由 `MockMapScene.cpp` 生成，来源显式为 `mock`。这不代表 2 号的真实瓦片、3 号路线结果或 4 号真实导航快照已经接入。

## 构建与测试

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++-13
cmake --build build -j1
ctest --test-dir build --output-on-failure
```

渲染测试：

```bash
QT_QPA_PLATFORM=offscreen ./build/src/pal/linux/renderer/p1-renderer-tests
```

开发期可选构建 A/B 对照工具：

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++-13 \
  -DP1_BUILD_RENDERER_BENCHMARK=ON
QT_QPA_PLATFORM=offscreen ./build/src/pal/linux/renderer/p1-renderer-benchmark
```

该工具默认关闭。普通构建、运行和演示不执行性能比较；Windows 或未使用统一 Ubuntu 环境的结果不能替代 Linux 验收数据。
