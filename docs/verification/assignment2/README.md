# 第二次作业：1 号首轮集成验证

日期：2026-11-01（Asia/Shanghai）。范围：应用装配、页面生命周期、已有模型加入总工程；不覆盖真实 A*/HMM、LinuxRenderer 或业务性能。对应 [集成说明](../../assignment-2-integration.md)。

## 环境与源码

- 本组 Git 基线 `ca12fe6` / `main`；本轮代码尚未提交。任务开始已有文档修改，未覆盖。代码文件指纹见 `source-files.sha256`。
- 实际来宾：VirtualBox `P1-Ubuntu-24.04`，Ubuntu 24.04.4 LTS amd64，内核 7.0.0-38-generic，1 vCPU / 8192 MiB RAM 配置，VMSVGA / 3D 关闭。根目录本轮为 `/dev/sda2 rw,relatime`；这次检查不解释或证明历史 emergency_ro 的根因已修复。
- Qt 6.4.2 / qt6-base-dev 6.4.2+dfsg-21.1build5；GCC/G++ 13.3.0 / 13.3.0-6ubuntu2~24.04.1；CMake 3.28.3 / 3.28.3-1build7；Ninja 1.11.1；Debug，`-j1`。
- QtTest 输出中“Qt library ... built by GCC 13.2.0”指系统 Qt 库的构建者；本项目编译器实测与 CMakeCache 为 `/usr/bin/g++-13` 13.3.0。
- 来宾独立根目录 `/home/navigation/p1-assignment2-20261101-member1-01`，基线源码 `baseline/`、本轮源码 `current/`、构建 `build/`、安装 `install/bin/p1-navigation`；未覆盖已有工程或删除其构建目录。
- 基线包来自 `git archive HEAD CMakeLists.txt src include tests tools`；本轮包来自相同目录的工作区。两份包不含 Git 历史、ai_log、私人凭据；包存于宿主忽略目录 `out/assignment2/` 和上述来宾目录。

| 包/程序 | SHA256 |
| --- | --- |
| baseline.tar | `7b03d4197f515630e479145b11f538b6c8f60769f6a09603814f79be1a6b22da` |
| source.tar.gz | `e7e0126354b187c9558a551010edb48b5df175f35aedbcc52c0f447b52a009d7` |
| install/bin/p1-navigation | `dd5071c93c40e2e396e5ab1a3b5b85fc2f4b957738faa00405394e134181bea8` |
| p1-navigation-assignment2-linux-amd64.tar.gz | `a540723bd042fb32da497905d36d5f2fb7a7ac83dece2ee32eb7304433f06f11` |

## 实际结果

| 检查 | 结果与证据 |
| --- | --- |
| 版本与文件系统 | 基线版本检查通过；见 environment.log、tools/verify-environment.sh |
| 修改前 UI | CTest 退出码 8；QtTest 6 通过、2 失败：地图入口被演示页启用，页面数量 4 与预期 3 不符。见 baseline-ui.log；只针对本轮 ca12fe6，不改写历史验收 |
| 本轮配置/完整构建 | 退出码 0；源码编译 warning 计数 0；见 configure.log、build.log |
| 根 CTest | 6/6 通过：ui-contracts、member3-models、navigation-location、navigation-location-demo、navigation-contracts、navigation-location-fixtures；见 ctest.log 和 LastTest.log |
| UI（offscreen） | 11 通过、0 失败，包含 Qt 初始化/收尾；见 LastTest.log |
| UI（真实 Wayland 会话） | 11 通过、0 失败；见 wayland-ui.log |
| 启动/安装/依赖 | 构建程序和安装程序 offscreen smoke 退出码 0，安装程序 Wayland smoke 退出码 0；ldd 无 not found；见 smoke.log、install.log、installed-smoke.log、wayland-smoke.log、ldd.log |
| BUILD_TESTING=OFF | 配置/完整构建退出码 0，编译 warning 计数 0，CTest 注册 0 项；见 no-tests-configure.log、no-tests-build.log、no-tests-ctest.log |
| 图像 | offscreen 与 Wayland 各生成浅色导入、深色导入、浅色组件、1024×720 紧凑窗口抓图；保留本轮 Wayland 抓图于 wayland-captures/ |
| Navit | 未安装，未建立版本/配置/输入基线；[NAVIT-20261101-1-integration-shell-01](../../benchmarks/navit/NAVIT-20261101-1-integration-shell-01.md) 为受阻（未测） |

Wayland 程序运行有 Mesa/EGL 初始化提示（含 ZINK failed to choose pdev）；offscreen 有平台 propagateSizeHints 提示。测试通过不代表 OpenGL ES 上下文、硬件加速或真实渲染后端通过。源码“零编译警告”不包含上述运行日志提示。

## 复现与证据

本目录 `verify.sh`、`verify-desktop.sh` 是实际执行脚本，先后运行。脚本使用上述独立路径且首次创建子目录；复测时改为新的空目录，先将两份源码包复制进去，再串行执行。不要删除已有验证目录来腾出同名路径。

首次抓图发现紧凑侧栏工具标题截字、状态栏折行，修正后执行 `finalize.sh`：更新本轮源码、核对 57 文件 SHA256、重建受影响目标、重跑完整 CTest 与 Wayland UI、重新抓图和安装打包。下列日志与图像保存最终结果；baseline-ui.log 仍保留修改前失败。最终包与源码指纹见 final-package-hashes.log、source-verification.log。首次源码包指纹为 `34b5679ec499de75fe206406a1b03dae35667d4b9fc816c36d85957c993e386f`，不与最终包混用。一次证据采集命令因只匹配 CMakeCache 的 FILEPATH 字段而中止，实际编译器字段为 STRING；补正采集后记录于 environment.log，未影响构建测试结果。

核心命令为：

```bash
bash current/tools/verify-environment.sh
cmake -S current -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++-13
cmake --build build -j1
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/install"
QT_QPA_PLATFORM=offscreen ./install/bin/p1-navigation --smoke
XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 \
  QT_QPA_PLATFORM=wayland ./build/p1-ui-tests
```

Wayland 检查必须在实际桌面用户会话和对应 socket 下运行，不能用 SSH 的 `XDG_SESSION_TYPE=tty` 判断桌面协议。窗口抓图来自 Wayland Qt 控件的 `grab()`，不是整屏桌面截图；抓图尺寸是测试明确指定的逻辑尺寸。

以上结果只验证应用外壳、接入入口和现有测试。真实业务不阻塞 UI、取消/过期算法结果、算法正确性、瓦片/路线/标记绘制、地图交互、200% 字号/高 DPI、Navit 性能对标仍待对应实现后验证。1 号未替其他成员声明模块验收完成。

AI 日志：对话 10（`20261101-member1-assignment2-integration-01`）。
