# p1_data_osmx：OSMX 统一数据解析库（2 号）

OSMX 是清洗后的二进制路网格式（`OSMX` 魔数、v1），本模块把它读成不可变的 `OsmxDataset`，预构建节点/道路/关系索引、节点→道路关联、有向边和最近节点网格，供 3 号 A*/HMM 与 5 号 LinuxRenderer 读取**同一份**数据。

- 格式与字段语义：[docs/osmx-format.md](../../../docs/osmx-format.md)
- 存储/清单约定：[docs/osmx-storage.md](../../../docs/osmx-storage.md)
- 解析与查询接口契约：[docs/osmx-api.md](../../../docs/osmx-api.md)

## 目标

| 目标 | 类型 | 说明 |
| --- | --- | --- |
| `p1_data_osmx` | STATIC | 仅依赖 Qt 6 Core；无 Widgets/libosmium/SQLite |
| `p1-osmx-cli` | 可执行 | 诊断：打印统计/警告，`--header-only` 只校验头部 |
| `p1-osmx-tests` | 测试 | CTest 名 `data-osmx`；真实文件用 `P1_OSMX_REAL_FILE` 启用 |
| `p1-import-page-tests` | 测试 | CTest 名 `import-page-osmx`；定义在 `src/app/`，验证页面导入与缓存重开 |

## 文件

```text
OsmxTypes.h/.cpp      错误码、边界、道路分类/标志、球面距离
OsmxDataset.h/.cpp    不可变数据集、ID 索引、关联/边 CSR、节点网格、查询
OsmxParser.h/.cpp     内存映射解析、校验、进度/取消、SHA-256、警告
OsmxCache.h/.cpp      原子缓存安装与清单扫描
OsmxStore.h/.cpp      进程内共享只读数据集（线程安全）
tools/OsmxCli.cpp     诊断命令行
tools/verify-osmx.sh  Ubuntu 24.04 一键复验脚本（构建/CTest/真实文件）
tests/OsmxTests.cpp   合成/损坏/真实文件测试
```

导入页面（`p1_import_page`）通过 `src/app/OsmxImportAdapter.*` 调用本模块，解析/缓存成功后 `publish()` 到 `OsmxStore`；页面本身不解析字节。重启后可用 `src/app/OsmxDatasetLoader.*` 从缓存重新打开同一数据集（ID/导入时间不变）。

## 构建与测试

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/g++-13
cmake --build build -j2
ctest --test-dir build --output-on-failure -R 'data-osmx|import-page-osmx'

# 一键复验（版本、构建、测试、真实文件、峰值内存）
src/data/osmx/tools/verify-osmx.sh --file /path/test.osmx

# 可选：解析真实 47 MiB 文件
P1_OSMX_REAL_FILE=/path/test.osmx build/src/data/osmx/p1-osmx-tests
build/src/data/osmx/p1-osmx-cli /path/test.osmx
build/src/data/osmx/p1-osmx-cli /path/test.osmx --header-only
```

## 状态与限制

- 2026-11-02：实现完成；开发机上 `p1-osmx-tests` 18 通过 / 0 失败 / 1 跳过（真实文件用例单独执行通过），新增 `p1-import-page-tests` 2 个用例通过；真实 `test.osmx` 统计与独立核对一致（见 API 文档第 9 节）。
- 当前验证在 Windows 11 / Qt 6.11 / GCC 13.1 开发主机完成；**Ubuntu 24.04 / Qt 6.4.2 构建与 CTest 待复验**，不能把开发机结果当作目标环境验收。复验步骤与通过标准见 [2 号验证记录](../../../docs/verification/assignment2-member2/README.md)。
- 解析器严格拒绝悬空道路引用、重复 ID、奇数/非法 UTF-16、尾随数据；关系成员允许指向裁剪范围外并给出警告。
- 数据不含时间、交通、转向限制；A*/HMM/渲染策略与实现不在本模块。
