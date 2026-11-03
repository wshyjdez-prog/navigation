# 第二次作业 2 号（统一数据）验证记录

范围：`p1_data_osmx`（OSMX 解析库）、`p1_import_page`（导入页面与工厂）、页面级 OSMX 测试与真实 `test.osmx` 检查。

状态（2026-11-02）：**Ubuntu 24.04 / Qt 6.4.2 验收未执行**。本轮可用的构建环境只有 Windows 开发主机（Qt 6.11.0 / MinGW GCC 13.1.0 / CMake 3.30.5 / Ninja 1.12.1），因此下表把“开发机检查”和“目标环境验收”分开记录，不把前者当作后者。

## 1. 为什么仍未在 Ubuntu 上执行

本轮开始时统一虚拟机不可从当前主机访问（无 VirtualBox 控制台入口、无 SSH 配置或凭据、无 WSL 发行版），无法在 Ubuntu 24.04 上运行 CMake/CTest 或桌面导入。按 `AGENTS.md` 第 5 节，模块状态保持“进行中”，不标“已完成”。

恢复条件：在统一 Ubuntu 24.04 虚拟机（Qt 6.4.2、GCC/G++ 13.3.0、CMake 3.28.3、Ninja）中克隆/复制本分支，执行第 3 节命令，把 `build-osmx/verify-osmx.log` 与本目录一并保存。

## 2. 本轮已完成的检查（开发机，非验收）

环境：Windows 11、Qt 6.11.0（`D:/Qt/6.11.0/mingw_64`）、MinGW GCC 13.1.0、CMake 3.30.5、Ninja 1.12.1、Debug。

| 检查 | 命令/入口 | 结果 |
| --- | --- | --- |
| 构建 | `cmake --build .scratch/full-cmake-sim/build`（根 CMake 的等价装配；`p1_data_import` 用 `.scratch` 桩替代 libosmium） | 50/50 目标成功，0 error、0 warning |
| OSMX 单元测试 | `p1-osmx-tests` | **18 通过 / 0 失败 / 1 跳过**（跳过项为需 `P1_OSMX_REAL_FILE` 的真实文件用例） |
| 页面级 OSMX 测试 | `p1-import-page-tests`（CTest `import-page-osmx`） | **2/2 通过**（新增） |
| 真实文件用例 | `P1_OSMX_REAL_FILE=test.osmx p1-osmx-tests parsesRealFileWhenProvided` | 通过（2.7 s） |
| 真实文件头部校验 | `p1-osmx-cli test.osmx --header-only` | 通过 |
| 真实文件完整解析 | `p1-osmx-cli test.osmx`，3 次 | 2,871 / 2,693 / 2,691 ms（解析+建索引） |
| 解析结果 | 同上 | 912,730 节点 / 121,964 道路 / 1,658 关系 / 44,986 可导航道路 / 536,688 有向边 / 16,408.873 km；SHA-256 `a9c4d90b…6467` 与清单一致 |

说明：`.scratch/full-cmake-sim` 只替换了 libosmium 解析目标（宿主无该依赖），`p1_data_osmx`、`p1_import_page`、`MainWindow`、UI 测试均使用仓库真实源码与真实根 CMake 片段。该装配不能替代 Ubuntu 根工程构建。

## 3. 目标环境复现步骤

在统一 Ubuntu 24.04 虚拟机、仓库根目录执行：

```sh
chmod +x src/data/osmx/tools/verify-osmx.sh
src/data/osmx/tools/verify-osmx.sh --file /path/to/test.osmx
```

脚本会依次完成：版本核对 → `cmake -G Ninja -DCMAKE_BUILD_TYPE=Debug` → 全量构建 → `ctest -R 'data-osmx|import-page-osmx'` → 真实文件头部/完整解析（`/usr/bin/time -v` 记录峰值内存）→ 真实文件单元用例；日志写入 `build-osmx/verify-osmx.log`。

> 注意：该脚本本轮**未在 Ubuntu 上实际执行过**（无环境），只做过逐行审阅；首次执行若遇路径/权限差异，请按实际环境调整并把完整日志留档，不要修改脚本后不留记录。

分步等价命令：

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/g++-13
cmake --build build -j2
ctest --test-dir build --output-on-failure -R 'data-osmx|import-page-osmx'
P1_OSMX_REAL_FILE=/path/to/test.osmx build/src/data/osmx/p1-osmx-tests
build/src/data/osmx/p1-osmx-cli /path/to/test.osmx --header-only
/usr/bin/time -v build/src/data/osmx/p1-osmx-cli /path/to/test.osmx
QT_QPA_PLATFORM=offscreen build/src/app/p1-import-page-tests
```

Release 复验（按团队惯例同时执行）：

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/g++-13
cmake --build build-release -j2
ctest --test-dir build-release --output-on-failure
```

## 4. 通过标准

- configure / build 退出码 0，`-Wall -Wextra -Wpedantic` 下 0 warning；
- `data-osmx`、`import-page-osmx`、`ui-contracts` 全部通过，无 skip（设置 `P1_OSMX_REAL_FILE` 后真实文件用例应执行并通过）；
- 真实文件统计与本文件第 2 节一致，SHA-256 相同；
- 记录 `/usr/bin/time -v` 的 `Maximum resident set size` 作为解析峰值内存（本轮宿主未测得，标“未测”，不以估计值代替）；
- 桌面导入截图（可选）：真实 Wayland 下导入 `test.osmx`，显示节点/道路/关系、可导航道路、有向边与警告。

## 5. 仍需在目标环境补测

| 项目 | 原因 |
| --- | --- |
| Qt 6.4.2 编译兼容性 | 开发机为 Qt 6.11；代码只使用 Qt 6.0+ Core/Widgets API，但未在 6.4.2 实际编译 |
| 真实根工程构建 | 开发机缺 libosmium/SQLite/EXPAT/BZip2，使用桩替代 |
| 桌面导入与主题切换 | 无 Wayland 桌面环境 |
| 峰值内存、重启后从缓存重新加载的实机场景 | 需真实数据与可交互桌面 |
| Navit 同源对标 | Navit 基线与同源输入仍缺（`NAVIT-20261102-2-osmx-import-01`，受阻未测） |

2026-11-03团队整合补测：OSMX源代码与实际导入页已纳入根工程，统一Ubuntu/Qt6.4.2中解析测试18通过、1项真实样例因未提供而跳过；页面offscreen/Wayland均4通过（含初始化/收尾）。完整工程CTest17/17。原Windows证据与本次Linux证据分别保留，详见 [团队整合验证](../assignment2-team/README.md)。
