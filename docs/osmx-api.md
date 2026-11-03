# OSMX 数据接口契约（2 号 → 3/5 号）

本文是 `p1_data_osmx` 的公开接口说明，供 **3 号 A*/HMM** 与 **5 号 LinuxRenderer** 读取同一份 OSMX 数据。格式见 [OSMX 文件格式](osmx-format.md)，落盘见 [OSMX 数据存储约定](osmx-storage.md)。**本文只描述数据与解析能力，不包含 A*、HMM、渲染或页面装配实现**；算法/渲染由对应负责人实现。

状态：2026-11-02 实现并通过合成数据 19 项测试及真实 `test.osmx` 解析检查；Ubuntu 24.04 / Qt 6.4.2 的复验仍由 2 号/1 号执行（本机验证环境见第 9 节）。接口字段若变更，必须同步本文与 3/5 号。

## 1. 模块、目标与依赖

| 项目 | 内容 |
| --- | --- |
| 解析库目标 | `p1_data_osmx`（STATIC） |
| 源目录 | `src/data/osmx/` |
| 公开头文件 | `data/osmx/OsmxTypes.h`、`OsmxDataset.h`、`OsmxParser.h`、`OsmxCache.h`、`OsmxStore.h` |
| 依赖 | Qt 6 Core（`QString`/`QVector`/`QHash`/`QMutex`）；**不依赖** Qt Widgets、libosmium、SQLite、Expat、zlib/bzip2 |
| 导入页面目标 | `p1_import_page`（STATIC，公开工厂 `src/app/ImportFactory.h`），依赖 `p1_data_osmx`、`p1_data_import`、`p1_ui_common`、Qt Widgets/Concurrent |
| 测试目标 | `p1-osmx-tests`，CTest 名 `data-osmx` |
| 诊断工具 | `p1-osmx-cli`（非应用组件） |
| 构建 | 根 CMake 已包含 `add_subdirectory(src/data/osmx)` 与 `add_subdirectory(src/app)`；`BUILD_TESTING=OFF` 时不构建测试 |

Ubuntu 24.04 统一环境（Qt 6.4.2 / GCC 13.3.0 / CMake 3.28.3）构建：

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++-13
cmake --build build -j2
ctest --test-dir build --output-on-failure -R data-osmx
# 真实文件检查（可选，文件不入库）：
P1_OSMX_REAL_FILE=/path/test.osmx build/src/data/osmx/p1-osmx-tests
```

## 2. 数据模型

### 2.1 实体

| 结构 | 字段 | 说明 |
| --- | --- | --- |
| `OsmxNode` | `id`, `lat`, `lon`, `tagFirst`, `tagCount` | `id` 为原始 OSM ID；经纬度 WGS84 度；标签切片指向 `nodeTagPool()` |
| `OsmxWay` | `id`, `refFirst`, `refCount`, `tagFirst`, `tagCount`, `minLat/minLon/maxLat/maxLon`, `hasGeometry`, `attributes` | `refFirst` 指向 `wayNodePool()`（元素是节点**索引**，不是 ID）；`attributes` 为预计算道路属性 |
| `OsmxRelation` | `id`, `memberFirst`, `memberCount`, `tagFirst`, `tagCount` | 成员切片指向 `memberPool()` |
| `OsmxMember` | `type`, `ref`, `role` | `type` 为 `Node/Way/Relation`；`role` 为字符串池索引 |
| `OsmxTag` | `key`, `value` | 两个字符串池索引，`kOsmxNoString` 表示缺失（正常标签不会出现） |
| `OsmxEdge` | `from`, `to`, `way`, `segment`, `lengthMeters` | 有向移动；`from/to` 是节点索引，`way` 是道路索引，`segment` 是该道路上第几个相邻段（0 起），长度米 |
| `RoadAttributes` | `roadClass`, `flags`, `lanes`, `maxSpeedKmh`, `layer`, `lengthMeters`, `name` | 见 2.3 |

所有实体 ID 使用 `qint64`（有符号 64 位原始 OSM ID）；数组索引使用 `quint32`。**两者不可混用**：查表用 ID，遍历/拓扑用索引。

### 2.2 单位与约定

| 量 | 单位/约定 |
| --- | --- |
| 坐标 | WGS84 经纬度，度；北纬/东经为正 |
| 距离/长度 | 米（`double`；`OsmxEdge::lengthMeters` 与 `RoadAttributes::lengthMeters` 为 `float`，精度约 0.1 m 内，导航累计请用 `double` 或重新按节点坐标计算） |
| 速度 | `RoadAttributes::maxSpeedKmh` km/h；0 = 未标注，**不是** 0 km/h |
| 方向 | `RoadAttributes` 的单行标志；不提供方位角，需要时由相邻节点坐标计算 |
| 时间 | 数据本身不含时间；`info().importedAt` 是导入时间，不是路网时间 |
| 时间戳 | ISO-8601 UTC 字符串（仅元数据） |

`OsmxDataset::bounds()` 是节点实际覆盖范围（WGS84）；`OsmxWay` 的 bbox 用于渲染裁剪，可能因缺失坐标而为空（本格式要求道路引用完整，正常不会发生）。

### 2.3 `RoadAttributes`

| 字段/方法 | 含义 |
| --- | --- |
| `roadClass` | 封闭枚举，`roadClassFromHighway()` 从 `highway` 标签映射；`roadClassName()` 返回原始字符串 |
| `flags` | `RoadBridge`、`RoadTunnel`、`RoadOneway`、`RoadOnewayReversed`、`RoadRoutable`、`RoadAccessDenied`、`RoadArea`、`RoadIncompleteGeometry` |
| `isRoutable()` | 可参与导航图；已排除面要素、禁止通行、几何不足 |
| `isOneway()` / `isOnewayReversed()` | 单行及方向；双向道路两者均为 false |
| `lanes` | 0 = 未标注；否则 1–255 |
| `maxSpeedKmh` | 0 = 未标注；否则 1–300 |
| `layer` | 整数层，桥/隧道/地下通道分层用；-100–100 |
| `lengthMeters` | 该道路所有相邻节点大地线距离之和；不包含转向代价 |
| `name` | 字符串池索引；`displayName()` 返回 `name:zh` > `name` > `name:en` 的显示名 |

`RoadClass` 为 `unknown` 或 `construction/raceway/bus_guideway` 时不进入有向图；其余分类是否允许特定交通方式由 3 号策略决定（例如是否允许 `footway` 步行、`motorway` 机动车）。

## 3. 解析接口（`OsmxParser`）

```cpp
#include "data/osmx/OsmxParser.h"
using namespace p1::data;

OsmxParseOptions options;
options.buildSpatialIndex = true;      // 默认 true
options.datasetId = QString();         // 空则生成 UUID
options.importedAt = QString();        // 空则用当前 UTC 时间；重开缓存时传清单值
options.rejectTrailingBytes = true;    // v1 严格

OsmxParseCallbacks callbacks;
callbacks.progress = [](int percent, const QString& stage) { /* 0..100，调用线程 */ };
callbacks.log = [](const QString& message) { /* 可选日志 */ };
callbacks.cancelled = [] { return /* 用户取消? */; };

const OsmxParseResult result = OsmxParser().parse(filePath, options, callbacks);
if (!result.ok()) {
    // result.error.code / message / offset；result.cancelled 表示取消
} else {
    std::shared_ptr<const OsmxDataset> dataset = result.dataset;
}
```

| 接口 | 说明 |
| --- | --- |
| `OsmxParser::inputFormat(path)` | `.osmx` → `"osmx"`，否则空串（按扩展名，读文件前可用） |
| `OsmxParser::supports(path)` | 是否为本格式 |
| `OsmxParser::validateHeader(path)` | 只读前 1 MiB 校验头部，不解析对象表；用于选择文件/诊断 |
| `OsmxParser::parse(...)` | 完整解析；成功返回 `dataset`，失败返回 `OsmxError`，取消时 `cancelled=true`；**不抛异常** |

`OsmxParseResult` 字段：`dataset`、`error`、`sourcePath`、`sha256`、`fileBytes`、`elapsedMs`、`warnings`、`cancelled`。

`OsmxParseOptions`：`buildSpatialIndex`（默认 true）、`datasetId`（空则生成 UUID）、`importedAt`（空则当前 UTC 时间；从缓存重新打开时传清单中的原导入时间，保持数据集元数据稳定）、`rejectTrailingBytes`（v1 默认严格）。`validateHeader()` 会按头部实际长度读取（来源路径最长 1 MiB），不依赖固定前缀大小。

错误码与触发条件见 [格式文档第 4 节](osmx-format.md#4-校验与错误模型)。调用方应：

- 在**工作线程**调用（`ImportTask` 已这样做），大文件不要放 GUI 线程；
- 检查 `ok()`，不要假设 `dataset` 非空；
- 把 `warnings` 展示/记录，不要静默丢弃（关系成员缺失、几何不足等）；
- 取消通过回调轮询，解析器会尽快返回 `Cancelled` 且不发布数据集。

## 4. 查询接口（`OsmxDataset`）

`OsmxDataset` 创建后**不可变**，所有方法为只读，可从任意线程并发调用；通过 `std::shared_ptr<const OsmxDataset>` 共享。数组访问不做加锁。

### 4.1 元数据

| 方法 | 返回 |
| --- | --- |
| `info()` | `OsmxDatasetInfo`：ID、来源、SHA-256、导入时间、格式版本、字节数、边界、标签/引用计数、可导航道路数、无向段数、有向边数、道路总长、警告 |
| `bounds()` | `OsmxBounds`（`valid`、min/max 经纬度） |
| `nodeCount()` / `wayCount()` / `relationCount()` | `quint32` |
| `hasSpatialIndex()` | 是否构建了节点网格 |

### 4.2 实体访问

| 方法 | 说明 |
| --- | --- |
| `nodeAt(i)` / `wayAt(i)` / `relationAt(i)` / `edgeAt(i)` | 按索引直接访问；越界是调用错误（先用 `hasNode/hasWay/hasRelation`） |
| `nodeIndex(id)` / `wayIndex(id)` / `relationIndex(id)` | ID → 索引，`std::optional<quint32>` |
| `wayNodePool()` | 全部道路节点索引；用 `way.refFirst/refCount` 切片 |
| `wayNodeIndices(wayIndex)` / `wayNodeIds(wayIndex)` | 复制出节点索引/节点 ID 列表（小数据便利接口） |
| `nodeTagPool()` / `wayTagPool()` / `relationTagPool()` / `memberPool()` / `strings()` | 原始池，避免逐对象复制 |
| `stringAt(i)` / `stringView(i)` | 字符串池访问 |
| `nodeTags(i)` / `wayTags(i)` / `relationTags(i)` / `relationMembers(i)` | 复制出标签/成员列表 |
| `nodeTag(i, key)` / `wayTag(i, key)` / `relationTag(i, key)` | 按 key 查值；`nullopt` = 无此标签，空 `QString` = 标签值为空 |
| `nodeHasTag` / `wayHasTag` | 是否存在标签 |
| `displayName(wayIndex)` | 优先 `name:zh`、`name`、`name:en`；无则空串 |

### 4.3 拓扑

| 方法 | 说明 |
| --- | --- |
| `outgoingEdgeFirst(nodeIndex)` / `outgoingEdgeCount(nodeIndex)` | 该节点出边在 `edges()` 中的范围 |
| `edges()` | 全部有向边，已按 `from` 分组 |
| `incidentWayFirst(nodeIndex)` / `incidentWayCount` / `incidentWays(nodeIndex)` | 引用该节点的道路（按道路索引排序、去重） |

**A\* 遍历示例**：

```cpp
const auto ds = OsmxStore::shared().active();
if (!ds) return;
for (quint32 node = 0; node < ds->nodeCount(); ++node) {
    const quint32 first = ds->outgoingEdgeFirst(node);
    const quint32 count = ds->outgoingEdgeCount(node);
    for (quint32 i = 0; i < count; ++i) {
        const OsmxEdge& edge = ds->edges().at(static_cast<int>(first + i));
        const OsmxNode& to = ds->nodeAt(edge.to);
        const RoadAttributes& way = ds->wayAt(edge.way).attributes;
        const double cost = edge.lengthMeters; // 3 号可结合 way.roadClass/maxSpeedKmh 调整
        Q_UNUSED(to); Q_UNUSED(way); Q_UNUSED(cost);
    }
}
```

- 双向道路已生成正反两条边，单行只生成允许方向，**无需再判断 `oneway`**。
- 边长度不含转向/坡度/交通；路由权重、启发函数、限行策略由 3 号实现。
- `RoadClass` 与 `maxSpeedKmh` 可用于车辆类型过滤和默认速度；0 表示未标注，需 3 号给默认值。
- 节点索引连续，可直接作为数组下标；若需要按 OSM ID 建图，用 `nodeAt(i).id`。

### 4.4 空间查询

| 方法 | 说明 |
| --- | --- |
| `nearestNode(lat, lon, maxDistanceMeters, double* distanceMeters)` | 在节点网格中找最近节点；`nullopt` = 无索引/超出范围；距离米 |
| `waysInBounds(area, limit, routableOnly)` | 返回 bbox 与区域相交的道路索引；`limit=0` 不限；渲染按需限制 |

节点网格单元约 0.005°（武汉纬度约 0.5 km），`nearestNode` 在查询点周围按 `maxDistanceMeters` 扩展搜索。搜索半径上限为 `OsmxDataset::kNearestNodeMaxGridRadius`（64 个单元，约 35 km），更大的 `maxDistanceMeters` 实际按该上限处理；它用于把定位点吸附到路网，不作为全局最近搜索。**只提供节点查询**，不提供“最近边/候选路段”接口；HMM 的候选边索引由 3 号按 `edges()` 自建（建议按边 bbox 或几何网格），解析器不替 3 号选择匹配算法。

渲染示例：

```cpp
OsmxBounds view{minLat, minLon, maxLat, maxLon, true};
const QVector<quint32> ways = ds->waysInBounds(view, 20000);
for (const quint32 w : ways) {
    const OsmxWay& way = ds->wayAt(w);
    if (!way.hasGeometry) continue;
    // 3/5 号按 way.attributes.roadClass / Bridge / Tunnel / layer 分层绘制
    for (quint32 i = 0; i < way.refCount; ++i) {
        const OsmxNode& node = ds->nodeAt(ds->wayNodePool().at(way.refFirst + i));
        // node.lat/lon 交给 5 号坐标变换；本接口不生成屏幕坐标/瓦片
    }
}
```

## 5. 共享实例（`OsmxStore`）

```cpp
#include "data/osmx/OsmxStore.h"
auto dataset = p1::data::OsmxStore::shared().active();
if (!dataset) { /* 尚未导入 */ }
const QString id = dataset->info().datasetId;
```

| 方法 | 说明 |
| --- | --- |
| `shared()` | 进程级唯一实例 |
| `publish(shared_ptr<const OsmxDataset>)` | 发布并设为活动数据集；空指针忽略 |
| `active()` | 当前数据集快照；无则空 |
| `find(datasetId)` | 按 ID 取快照；保留最近 4 个非活动数据集 |
| `activeId()` / `datasetIds()` | 元信息 |
| `clear()` | 测试/会话清理；已持有的快照仍有效 |

**保证**：导入页成功导入后调用 `publish()`；3 号与 5 号读取 `active()` 得到同一 `dataset` 指针。数据集不可变，消费者不必加锁，也不得修改。

### 5.1 重启后重新打开缓存数据集

进程重启后 `OsmxStore` 为空，但本地缓存仍在（见存储约定）。导入页提供“从本机加载所选数据集”，通过工厂取得 `OsmxDatasetLoader`：

```cpp
// entry 来自 OsmxCache::list(root).entries 或页面列表行
if (app::ImportFactory::canReopenCachedDataset(entry)) {
    auto loader = app::ImportFactory::createDatasetLoader(entry); // ImportParser
    // 交给 ImportTask 在工作线程执行；loader 只读 <entry.cachePath>/data.osmx，
    // 用清单 datasetId/importedAt 重新解析，校验 SHA-256 后 publish() 到 OsmxStore
}
```

- 数据集 ID 与导入时间沿用清单值，`OsmxStore::active()->info().datasetId` 在重启前后一致；
- 解析出的 SHA-256 与清单不一致、大小不符或文件缺失时返回错误且**不发布**，不覆盖本地数据；
- 该路径不写缓存，可重复调用；3/5 号不需要区分“新导入”与“重新打开”，都读 `active()`。

## 6. 本地缓存（`OsmxCache`）

| 方法 | 说明 |
| --- | --- |
| `defaultDirectory()` | `QStandardPaths::AppLocalDataLocation + "/imports"`（与标准 OSM 共用根） |
| `install(dataset, sourceFile, directory, callbacks)` | 原子写入 `<directory>/<datasetId>/{data.osmx,manifest.json}`；校验 SHA-256；失败/取消返回原因，不发布半成品 |
| `list(directory)` | 扫描清单，返回 `app::ImportResult` 列表 + `problems`；损坏条目跳过 |
| `dataPath/manifestPath/datasetDirectory` | 路径拼接 |
| `remove(directory, datasetId)` | 删除一个数据集目录 |
| `isSafeDatasetId(id)` | ID 是否可安全用作目录名 |

清单字段见 [存储约定第 3 节](osmx-storage.md#3-清单-manifestjson)。

## 7. 导入页面契约（给 1 号装配）

- 页面固定 ID：`ImportFactory::pageId()` = `"import"`；标题/说明由 `pageTitle()/pageDescription()` 提供。
- 工厂：`ImportFactory::createImportPage(theme, parent, cacheDirectory)`，**仅 GUI 线程**调用，跨线程返回 `nullptr`。
- 工厂（重新打开）：`ImportFactory::canReopenCachedDataset(entry)` / `createDatasetLoader(entry)`；非 OSMX 行或非法 ID 返回 `false`/`nullptr`，页面据此禁用按钮。
- 注册：`MainWindow::registerPage(ImportFactory::pageId(), ..., page)`；成功即由窗口堆栈接管所有权，失败不转移所有权。
- 页面用 `ImportPage::importFinished(ImportResult)` 汇报结果；`ImportResult.cachePath` 是数据集目录，`datasetId` 是共享键。
- `ImportPage::openSelectedDataset()` 加载“本地数据集”列表中选中的行（也可双击列表项）；结果是同一条 `importFinished` 信号，1 号无需为重新打开增加事件类型。
- 页面自己的 worker 线程（`ImportTask`）在页面存活期间可继续；析构时取消并等待。切换页面不销毁页面，导入不阻塞 GUI。
- 本模块**不**注册其他页面、不装配主窗口、不访问 3/5 号内部状态。

## 8. 边界：本接口不提供什么

- 不实现 A*、HMM、地图匹配或路径代价模型（3 号）。
- 不实现 LinuxRenderer、QPainter/OpenGL、瓦片、屏幕坐标变换（5 号）。
- 不提供实时交通、封路、转向限制、红绿灯等待、历史轨迹。
- 不做坐标系转换（GCJ02/BD09）；OSMX 数据固定 WGS84。
- 不提供数据库表；消费者不得直接读 `data.osmx` 原始字节。
- 不实现 G7/X07 离线包下载/增量更新/回滚（X07 未确认）。

## 9. 验证状态与证据

2026-11-02 在开发主机（Windows 11、Qt 6.11.0、GCC 13.1.0、Debug）实测：

- `p1-osmx-tests`：**18 通过 / 0 失败 / 1 跳过**（跳过项为需 `P1_OSMX_REAL_FILE` 的真实文件用例，单独执行时通过）；覆盖合成路网解析、标签/拓扑/单行/面积/短几何、进度单调、取消、头部校验、9 类损坏文件、缓存安装/列表/损坏跳过、Store 共享；
- 新增 `p1-import-page-tests`（CTest `import-page-osmx`）：**2 个用例通过**——通过真实 `ImportPage` 后台导入合成 `.osmx` 并校验 `OsmxStore` 活动快照；模拟重启后再从缓存重新打开同一数据集（ID/导入时间/SHA/拓扑一致）；
- 真实 `test.osmx`：与 Python 独立核对一致——912,730 节点、121,964 道路、1,658 关系、44,986 可导航道路、536,688 有向边，SHA-256 与边界一致；解析+建索引 2,691–2,871 ms（Debug，3 次，含 47 MiB 映射与哈希；**不是** Ubuntu 验收指标），峰值内存**未测**；
- `ImportPage + MainWindow` 冒烟：注册 ID `import`、后台导入、缓存发布、`OsmxStore` 活动快照、页面状态恢复均通过（Qt 6.11/offscreen，使用本地 stub 替代 libosmium，仅验证本模块路径）。

**限制**：上述为 Windows 开发主机检查，不是 Ubuntu 24.04 / Qt 6.4.2 验收；Ubuntu 构建/CTest、真实桌面截图、峰值内存与性能基线仍待 2 号/1 号执行，复现步骤见 [2 号验证记录](verification/assignment2-member2/README.md) 与 `src/data/osmx/tools/verify-osmx.sh`。解析器只使用 Qt 6.0+ Core API 与 C++17，未使用 Qt 6.5+ 接口。`test.osmx` 不入 Git，验收时按 SHA-256 提供。
