# 第二次作业团队整合（2026-11-03，组员 1）

本轮将 3 号 `feature/member3-open-source-integration/c7ee852` 与主线 `e3fb6a6` 合并，并把 2 号压缩包交付的 OSMX 源码纳入正式工程；4 号规划桥/真实 A* 对接和 5 号 LinuxRenderer 已在主线。整合不更换成员已确认的算法、接口或第三方来源。

## 正式入口

- `p1-navigation`：导入页同时支持标准 OSM/PBF/gzip/bzip2 与 OSMX；共享公共主题，后台解析，合并本地数据集目录。OSMX 可从缓存重新加载到不可变 `OsmxStore`；标准 OSM 保持 SQLite 原始数据缓存，不自动生成路由图。
- `p1_data_osmx` / `p1_import_page`：2 号库和实际页面进入根构建，不使用替代 libosmium 的 Windows 桩。
- `p1_algorithms_astar` / `p1_algorithms_route_planner`：3 号 Boost.Graph A* 和 Qt 异步接口；默认开启 `P1_NAVIGATION_REAL_PLANNER`，4 号对接演示/测试消费真实规划器。
- `p1_algorithms_fmm`：默认编译 FMM CLI 适配层；真实 FMM/HMM 运行另需固定版本可执行程序、同源网络和 UBODT，不能将进程 fixture 视为 HMM 验收。
- `p1_renderer` / `MapPage`：5 号异步 QImage 渲染与地图页面保留，输入仍明确标注 Mock。

## Ubuntu 构建与验证

统一 Ubuntu 24.04、Qt 6.4.2、G++ 13.3.0、CMake 3.28.3，C++17。在已有标准 OSM 依赖之外新增 Boost 1.83 头文件；可安装 Ubuntu `libboost1.83-dev`。本机无管理员权限，使用 `apt-get download` 与 `dpkg-deb -x` 解到用户 SDK，并通过 `BOOST_ROOT` 指定；不将 SDK/第三方源码提交仓库。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++-13 \
  -DCMAKE_PREFIX_PATH=/home/navigation/p1-deps/osmium-2.20/root/usr \
  -DBOOST_ROOT=/home/navigation/p1-deps/boost-1.83/root/usr
cmake --build build -j1
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
./build/p1-navigation
```

OSMX 与导入页样例覆盖解析、拓扑、错误、取消、缓存发布和模拟重启；标准 OSM 测试继续验证多格式与事务回滚；真实 A* 对接覆盖成功、取消、不可达和非节点端点。目标环境原始结果保存在 `docs/verification/assignment2-team/`，以实际结果为准。

## 范围与剩余工作

本次是交付物合并和共同构建/回归验证。各独立模块可共存编译不代表完整业务链已贯通：算法调用页面和性能面板、OSMX 到算法图及渲染场景的生产转换、真实 FMM 数据配置、完整导航状态机、GeoClue2/语音/G6 联调仍待对应成员推进。地图固定 Mock 不作为真实地图数据联调证据；真实 OSMX 样例未提供时保留测试跳过，不伪造结果。

历史 Navit 导入报告保留，未把旧数据改写为本轮新测量。此次仅合并现有算法与适配器，没有新增候选算法试验；整合后的完整业务及渲染/算法性能缺口另见 `NAVIT-20261103-1-team-integration-01`。
