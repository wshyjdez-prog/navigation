# OSMX 数据存储约定

本文规定 OSMX 统一数据在本机的落盘方式，供 2 号（导入/存储）、3 号（算法）、5 号（渲染）和 1 号（装配/分发）共同遵守。解析格式见 [OSMX 文件格式](osmx-format.md)，查询接口见 [OSMX 数据接口契约](osmx-api.md)。

状态：2026-11-02 随 `OsmxParser`/`OsmxCache` 实现。当前**不是**完整 StorageAdapter 或 G7/X07 离线包协议；X07 的外部方案仍未确认（见 `project.md` 第 5 节）。

## 1. 目录布局

OSMX 与标准 OSM 共用同一个导入根目录：

```text
<AppLocalDataLocation>/imports/          # Linux: ~/.local/share/<app>/imports（由 QStandardPaths 决定）
├── <datasetId>/                         # 一个 OSMX 数据集目录（UUID，无花括号）
│   ├── data.osmx                        # 原文件逐字节副本
│   └── manifest.json                    # 清单：版本、校验、统计、边界、警告
├── <datasetId>.sqlite                   # 标准 OSM 导入缓存（另一条路径，非本模块）
└── .staging-<uuid>/                     # 安装中的临时目录；成功后原子改名，不会出现在目录列表中
```

- 数据集 ID 由导入时生成（UUID v4，去花括号），同时作为目录名；只允许 `[A-Za-z0-9_-]`，长度 ≤ 64，拒绝 `..`、路径分隔符和隐藏目录。
- `data.osmx` 是源文件的完整副本，不是重新编码；`manifest.json` 是唯一可信的元数据入口。
- 不使用数据库：OSMX 是只读、自包含的路网文件，解析后构建内存索引（见 API 文档）。这样算法与渲染读取同一个不可变对象，不需要分别查询 SQLite。
- 标准 OSM 的 `.sqlite` 缓存仍按 [标准 OSM 导入](osm-import.md) 管理；两条路径只在目录扫描层合并显示。

## 2. 发布（安装）流程

`OsmxCache::install(dataset, sourceFile, root, callbacks)` 在导入工作线程执行：

1. 校验 `datasetId` 安全、根目录可创建、源文件可读、数据带 SHA-256、目标目录不存在。
2. 创建 `<root>/.staging-<uuid>/`。
3. 以 1 MiB 分块复制 `sourceFile → staging/data.osmx`，同时计算 SHA-256；每块检查取消。
4. 复制完成后核对 SHA-256 与 `dataset.info().sourceSha256` 一致，并复核源文件大小/修改时间未变；不一致则删除暂存目录并失败。
5. 用 `QSaveFile` 写 `staging/manifest.json`（提交失败不留下半个清单）。
6. `QDir::rename(staging, <root>/<datasetId>)` 原子发布。目标已存在则失败，不覆盖既有数据集。
7. 取消、I/O 错误、校验失败、改名失败都会递归删除暂存目录；**不会出现目录列表可见的半成品**。

进度：复制阶段 0–95%，清单 97%，发布 100%。这是“写入本地缓存”的进度，导入页会在解析进度之后继续显示。

## 3. 清单 `manifest.json`

`schema` 必须精确等于 `p1-osmx-cache/1`；未知 schema 的目录在扫描时跳过并记录问题。字段：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `schema` | string | `p1-osmx-cache/1` |
| `format` | string | `osmx` |
| `formatVersion` | number | 文件格式版本（当前 1） |
| `reader` | string | `p1 OsmxParser v1` |
| `name` / `sourceFile` | string | 源文件名（展示用） |
| `sourcePath` | string | 导入时的绝对路径（可能已不可用，仅追溯） |
| `headerSourcePath` | string | OSMX 头部记录的来源路径 |
| `datasetId` | string | 必须等于目录名 |
| `sha256` | string | 源文件/副本的 SHA-256 |
| `importedAt` | string | ISO-8601 UTC 时间 |
| `elapsedMs` | string | 解析+建索引+缓存写入的耗时（毫秒，字符串避免 JSON 精度问题） |
| `bytes` | string | 文件字节数 |
| `nodes` / `ways` / `relations` | string | 对象数量 |
| `tags` | string | 标签总数（节点+道路+关系） |
| `nodeTags` / `wayTags` / `relationTags` | string | 分类标签数 |
| `nodeReferences` | string | 道路节点引用总数 |
| `relationMembers` | string | 关系成员总数 |
| `routableWays` | string | 可导航道路数 |
| `undirectedSegments` | string | 全部道路的相邻节点段数（含建筑/水系等，不只导航道路） |
| `directedEdges` | string | 有向边数（A*/HMM 可直接遍历） |
| `roadLengthMeters` | string | 可导航道路几何长度（米，3 位小数） |
| `hasBounds` | bool | 是否有边界 |
| `minLat` / `minLon` / `maxLat` / `maxLon` | number | WGS84 边界 |
| `warnings` | array<string> | 解析警告原文（如关系成员缺失） |

示例（**仅示意字段形状，数值/时间/哈希不是真实产物**；真实值由导入生成）：

```json
{
  "schema": "p1-osmx-cache/1",
  "format": "osmx",
  "formatVersion": 1,
  "reader": "p1 OsmxParser v1",
  "name": "synthetic.osmx",
  "sourceFile": "synthetic.osmx",
  "sourcePath": "C:/fixtures/synthetic.osmx",
  "headerSourcePath": "C:/fixtures/synthetic.osmx",
  "datasetId": "cache-dataset",
  "sha256": "…",
  "importedAt": "2026-11-02T09:30:00.000Z",
  "elapsedMs": "…",
  "bytes": "…",
  "nodes": "5",
  "ways": "5",
  "relations": "2",
  "tags": "19",
  "nodeReferences": "12",
  "relationMembers": "3",
  "routableWays": "3",
  "undirectedSegments": "7",
  "directedEdges": "5",
  "roadLengthMeters": "464.98",
  "hasBounds": true,
  "minLat": 30.5,
  "minLon": 114.3,
  "maxLat": 30.503,
  "maxLon": 114.304,
  "warnings": []
}
```

## 4. 目录扫描与容错

`OsmxCache::list(root)`：

- 只扫描一级子目录，忽略隐藏目录（含 `.staging-*`）和普通文件。
- 每个目录必须同时具备可读的 `manifest.json`、合法 schema、`datasetId == 目录名`、可读的 `data.osmx`，且 `bytes` 与文件大小一致；否则跳过并在 `problems` 中说明原因，不影响其他数据集。
- 返回 `app::ImportResult` 值对象（与标准 OSM 列表同一类型），按 `importedAt` 倒序；页面据此生成“本地数据集”列表。
- 扫描只读清单，不解析 `data.osmx`；需要数据时再解析该目录下的 `data.osmx`（`OsmxDatasetLoader` 负责），避免启动时解析几十 MB。
- 删除数据集使用 `OsmxCache::remove()`，先校验 ID 安全，再递归删除目录；失败返回原因。

已知限制：扫描期间其他进程正在安装时，`.staging-*` 不可见，只有改名完成后才进入列表；若进程在复制中崩溃，遗留的 `.staging-*` 不会自动清理（后续可加启动清理，需另行实现）。

## 5. 生命周期与共享

```text
ImportPage 选择文件
  → ImportTask 工作线程
  → OsmxImportAdapter: OsmxParser::parse()        // 只读校验 + 内存索引
  → OsmxCache::install()                          // 原子写 <root>/<datasetId>/
  → OsmxStore::publish(dataset)                   // 进程内共享不可变快照
  → ImportResult 返回 GUI 线程，页面显示统计/警告
```

- `OsmxStore` 进程级单例，按数据集 ID 保留最近 4 个非活动数据集；活动数据集不会被淘汰。`publish()` 之后，3/5 号通过 `OsmxStore::shared().active()` 取得 `shared_ptr<const OsmxDataset>`，持有期间对象一直有效。
- 同一个解析结果只发布一次；消费者的任何读取都不会修改数据。需要另一个数据集时重新导入并发布，旧快照仍可被已持有者安全使用。
- 重启应用后列表来自清单；在页面“本地数据集”中选中一项并点击“从本机加载所选数据集”（或双击），`OsmxDatasetLoader` 会重新解析该目录的 `data.osmx`、核对清单 `sha256`/`bytes`，再 `publish()` 到 `OsmxStore`。数据集 ID 与 `importedAt` 沿用清单值，重启前后一致；加载不写缓存，可重复执行。应用启动时**不会**自动解析任何数据集（避免为未使用的数据付出几十 MB 的内存与时间）。

## 6. 校验、版本与离线包

- 完整性以 `sha256` 为准；复制后再次核对。缓存被篡改/截断时，`list()` 报大小不一致；直接解析会报 `truncated`/`bad_string` 等。
- `formatVersion` 与 `schema` 分离：OSMX 文件格式升级时升 `formatVersion`；清单结构变化时升 `schema`。读取方遇到未知值应跳过并提示重新导入，不猜测。
- 离线分发：复制 `<root>/<datasetId>/` 整个目录即可（两个文件）。接收方放到自己的导入根目录，启动后即可出现在列表；哈希以清单为准。当前**没有**实现 G7 要求的下载/增量更新/回滚协议，X07 未确认，不能把本目录约定当成完整离线包验收。
- 权限：缓存位于用户数据目录，沿用操作系统用户权限；不写入凭据、账号或私人轨迹。清单中的 `sourcePath` 可能包含用户名，分发前应评估脱敏（目前未自动脱敏）。

## 7. 与标准 OSM 缓存的关系

| 项目 | 标准 OSM | OSMX |
| --- | --- | --- |
| 文件 | `<uuid>.sqlite` | `<uuid>/data.osmx` + `manifest.json` |
| 解析 | libosmium 流式写 SQLite | 内存映射 + 定长记录解析 |
| 查询 | 每次打开只读 SQLite 连接 | 共享只读 `OsmxDataset` 内存快照 |
| 拓扑 | 原始引用，消费方自建 | 预构建节点→道路、节点→有向边、道路几何 |
| 适合 | 任意 OSM 快照/多格式 | 清洗后的统一路网，供算法/渲染直接消费 |
| 目录扫描 | `*.sqlite` | 子目录 `manifest.json` |

两者都通过 `app::ImportResult` 向页面汇报，页面无需区分存储细节。**不得**让算法/渲染直接读 `.sqlite` 私有表或 `data.osmx` 的原始字节；统一走本文与 API 文档定义的接口。
