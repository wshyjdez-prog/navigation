# LinuxRenderer A/B 模式与 Navit 对标记录

报告 ID：`NAVIT-20261101-5-renderer-01`

日期：2026-11-01（Asia/Shanghai）

负责人：5 号

对标状态：**本次取消性能比较；仅保留 Linux 功能验收**

## 1. 范围与用户决定

用户于 2026-11-01 明确要求移除界面中的 A/B 比较，只保留符合第二次作业需求的正常运行流程。因此：

- 地图页不再显示渲染模式选择；
- 正式页面固定使用缓存底图渲染；
- `Immediate` 代码仅作开发期正确性对照，不进入用户界面；
- 基准程序默认不构建，不需要用户运行；
- 不将 Windows 预检数据作为验收或性能结论。

## 2. 基线

代码基于最新 `main/cb056d0` 新建分支，并读取 Navit `trunk` 提交 `590b50a14731515307541b4f11ee51be9468e441` 的图形接口作为架构参考。Navit 源文件采用 GPL v2，本轮未复制源码。

由于 2/3/4 号真实数据接口尚未在此分支完成装配，测试输入为显式 `mock` 的 `mock-campus-v1`：1024×640 逻辑画布、8 张 256×320 瓦片、1 条路线、3 个标记。性能场景将路线扩展到 1024 个点。

## 3. 正确性

| 检查 | 状态 | 证据 |
| --- | --- | --- |
| A/B 输出尺寸与图层计数 | 通过 | `p1-renderer-tests` |
| A/B 相同输入输出一致 | 通过 | `p1-renderer-tests` |
| A 第二帧底图缓存命中 | 通过 | `p1-renderer-tests` |
| B 不使用底图缓存 | 通过 | `p1-renderer-tests` |
| 非法版本、空瓦片、坏坐标拒绝 | 通过 | `p1-renderer-tests` |
| 导航覆盖层消费 `NavigationSnapshot` | 通过 | `p1-renderer-tests` |
| 连续请求忽略陈旧帧 | 通过 | `p1-renderer-tests` |
| MapPage 单一入口异步首帧 | 通过 | Windows Qt 6.12 offscreen 冒烟 |

以上运行发生在 Windows + Qt 6.12.0 + MinGW GCC 13.1.0 的临时独立冒烟工程中，不是统一 Ubuntu/Qt 6.4.2 验收结果。

## 4. 已撤回的 A/B 预检

以下数据仅保留为开发历史，不是作业要求，也不在正常运行流程中展示。用户已取消比较，因此不再据此选择或宣传性能优势。

| 场景 / 指标 / 单位 / 方向 | A 缓存底图 | B 完整重绘 | 差值 A-B |
| --- | ---: | ---: | ---: |
| 中位数耗时 / µs / 越低越好 | 9263 | 9550 | -287（-3.0%） |
| P95 耗时 / µs / 越低越好 | 9691 | 9983 | -292（-2.9%） |
| 平均耗时 / µs / 越低越好 | 9269.48 | 9562.08 | -292.60（-3.1%） |
| 缓存命中 / 样本 | 50 / 50 | 0 / 50 | 不适用 |
| 峰值 RSS | 未测（Windows 脚本不读取 VmHWM） | 未测 | 不计算 |

预检显示当前小规模场景中 A 仅比 B 略快，不能宣称显著性能优势。导航覆盖层和图像复制仍占主要时间；路线复杂度或底图更新频率变化后需重新测量。

原始逐次样本：`evidence/windows-qt612-smoke.json`。

## 5. 与 Navit 的比较边界

Navit 的 Qt 后端通过 `graphics_methods` 暴露线条、图片、文字和 draw-mode 操作，并在 QWidget 中复制缓存位图和覆盖层。本实现借鉴了以下工程顺序：

1. 先准备可绘制数据，再开始一帧；
2. 按显示层顺序绘制；
3. 完成后提交整帧；
4. 将后台结果交回 UI 表面显示。

但当前项目没有与 Navit 相同的导航 bin、图层配置、地图符号集和交互输入，也没有在统一 Ubuntu 虚拟机运行 Navit。因此：

- 不与 Navit 计算帧耗时或内存比值；
- 不把 OSM 导入耗时当成渲染基线；
- 不声称本组 A/B 算法优于 Navit；
- Navit 对照任务须使用同区域、同视野、同缩放、同路线/标记密度、同分辨率和同图形后端，待 1 号共同基线建立后补测。

## 6. 后续 Linux 功能验收

```bash
cmake -S . -B build-release-renderer -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++-13
cmake --build build-release-renderer -j1
ctest --test-dir build-release-renderer --output-on-failure
./build-release-renderer/p1-navigation
```

普通验收只检查地图页能否异步显示瓦片、路线、标记和 Mock 导航信息，以及操作期间界面是否保持响应。`p1-renderer-benchmark` 默认不构建，无需执行。
