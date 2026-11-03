# 第二次作业团队整合：Ubuntu 实测

2026-11-03；组员 1。基线 `e3fb6a6`＋3 号 `c7ee852` 合并、2 号交付源码及公共装配修改。源码清单 [source-manifest.json](source-manifest.json) 共 122 文件，来宾核对 0 差异；详见 [source-check.txt](evidence/source-check.txt)。

| 检查 | 实际结果与原始证据 |
| --- | --- |
| 全新 Release 构建 | 141/141，成功；[build.txt](evidence/build.txt)，有 1 条既有未使用函数 warning（MapScene.cpp::pointText），未隐藏 |
| 最终页面说明调整后的增量构建 | 6/6 成功；[final-build.txt](evidence/final-build.txt) |
| 根 CTest | 首轮及最终各 17/17；最终 15.48 秒，[ctest-final.txt](evidence/ctest-final.txt)、[各用例输出](evidence/LastTest.txt) |
| UI / 标准 OSM / OSMX / 渲染 | 总数分别为 13/0、17/0、18/0/1跳过、8/0；均在完整工程真实依赖下运行，不使用 Windows libosmium 桩 |
| OSMX 导入页 | 4/0（含初始化/收尾），真实页面导入、Store 发布、缓存重开 |
| 真实 Wayland UI | 13/0；[wayland-ui.txt](evidence/wayland-ui.txt) |
| 真实 Wayland OSMX 页面 | 4/0；[wayland-osmx-page.txt](evidence/wayland-osmx-page.txt) |
| BUILD_TESTING=OFF | 配置、完整默认构建、应用启动成功；[no-tests-build.txt](evidence/no-tests-build.txt)、[启动](evidence/no-tests-smoke.txt)；随后已恢复 ON |
| 安装与运行库 | [安装](evidence/install.txt)、[已安装程序启动](evidence/installed-smoke.txt)、[ldd](evidence/ldd.txt) 无缺失依赖；桌面/菜单快捷方式指向本轮新版，见 [部署结果](evidence/deployment.json) |

环境：Ubuntu 24.04、Qt 6.4.2、G++ 13.3.0、CMake 3.28.3、Boost 1.83、1 vCPU/8 GiB、Wayland；[实际环境](evidence/environment.txt)。编译期间后台更新负载较高，不将编译时间用作性能结论。EGL/Mesa 提示保留在原日志，不证明硬件加速。最初发现遗留和本轮构建共用目录，已停止并从独立 `p1-team-final-20261103` 目录完整重建，最终证据仅来自独立目录。

限制：真实 `test.osmx` 未提供，`parsesRealFileWhenProvided` 明确跳过；本机无固定版本 FMM、真实网络与 UBODT，`member3-fmm-official` 未注册，FMM fixture 仅测试进程调度/回调，不是 HMM 正确性证明。A* 与导航桥测试使用固定样例路网。地图截图仍标注 Mock。

来宾安装入口：`/home/navigation/p1-team-final-20261103/install/bin/p1-navigation`。源码/构建在同目录的 `source/`、`build/`。桌面验证可复用 `tools/verify-team-desktop.py`；构建命令见 [团队整合说明](../../assignment-2-team-integration.md)。

图像已检查导入/地图明暗及紧凑布局；全部截图在 [captures](evidence/captures/)。标准湖北 SQLite 缓存仍可从启动列表读取；OSMX 列表项可从本机重开，PBF 行不冒充 OSMX 可路由数据。完整业务连通与性能缺口见 [本轮 Navit 报告](../../benchmarks/navit/NAVIT-20261103-1-team-integration-01.md)。
