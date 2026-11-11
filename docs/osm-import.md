# 标准 OSM 导入（组员 1 授权跨模块装配）

## 2026-11-11：改为 libosmium 要素导入（POI / 道路 / 面）

决策 `DEC-20261111-1-osm-feature-import-01`，1 号授权的跨模块修改（主责 2 号）。**本节取代下文 2026-11-01 的“原始 OSM 镜像”缓存设计**；下文保留作历史追溯。OSMX 入口（`src/data/osmx/`）不变。

### 做了什么

`OsmImportParser` 仍读 `.osm`、`.pbf`/`.osm.pbf`、`.osm.gz`、`.osm.bz2` 快照，但不再把节点/路/关系原样存表，而是用 libosmium 2.20.0 在导入时直接提取三类要素：

| 要素 | 判定规则（`src/data/import/FeatureRules.h`） | libosmium 组件 | 缓存表 |
| --- | --- | --- | --- |
| POI | 带 `amenity`/`shop`/`tourism`/`leisure`/`historic`/`office`/`healthcare`/`craft`/`emergency`/`public_transport` 的对象，以及 `railway=station/halt/tram_stop/subway_entrance`、`aeroway=aerodrome/terminal`；值为空或 `no` 不算。节点取自身坐标，闭合路/multipolygon 取外包矩形中心 | `osmium::handler::Handler`、`osmium::Area::envelope()` | `pois` |
| 道路 | 带非空 `highway` 的路，至少 2 个节点且全部有坐标；缺节点的路整条拒绝，不补造连接 | `NodeLocationsForWays`（给路的节点引用附坐标） | `roads` |
| 面 | 建筑、水域、绿地、土地利用（规则与原 `MapAreas.cpp` 相同） | `osmium::area::MultipolygonManager` + `osmium::area::Assembler`（闭合路与 multipolygon 的外环/内洞组装） | `areas` |

**POI 只入库，不在地图上显示。** 地图金字塔只消费 `visitMapRoads` / `visitMapAreas`；`tests/TilePyramidTests.cpp` 在岛中心放了一个 POI 并断言该像素仍是陆地/建筑底色。POI 通过 `ImportedDataset::pois()`（矩形 + 可选类别）和 `findPois()`（名称子串）读取，供后续搜索/列表使用。

### 读取流程

1. 计算源文件 SHA256（进度 0–10%）。
2. 第一遍只读关系，让 `MultipolygonManager` 记下 multipolygon 的成员路（10–20%）。
3. 第二遍读全部对象（20–88%），处理链为：节点校验与 POI → `NodeLocationsForWays` → 道路 → 面组装回调。节点坐标索引正 ID 用 `SparseFileArray`（暂存目录内的内存映射文件，不占常驻内存），负 ID（编辑器本地对象）用 `SparseMemArray`。
4. 排序后扫描坐标索引拒绝重复节点 ID；统计缺失引用、不完整 multipolygon、无法成环的面并写成警告。
5. 建普通 B-tree 索引 `roads(min_lat,max_lat,min_lon,max_lon)`、`pois(lat,lon)`、`pois(category)`，单事务提交后原子重命名发布。取消或出错不发布。

### 缓存 schema 2（私有，消费者只走公共契约）

- `pois(id, osm_type, osm_id, lat, lon, category, kind, name, tags)`：`osm_type` 0 节点 / 1 路 / 2 关系；`tags` 为 `[[key,value],…]` JSON。
- `roads(id=路 ID, highway, name, tags, min_lon, min_lat, max_lon, max_lat, refs, coords)`：`refs` 是节点 ID 序列（保持拓扑，供 A*/HMM 建图），`coords` 是对应的 (lon, lat)。
- `areas(id, osm_type, osm_id, kind, name, min_zoom, min_lon, min_lat, max_lon, max_lat, geometry)`：每个外环一行，`geometry` = 外环 + 该外环的内洞。
- 几何 BLOB 为 `QDataStream`（`Qt_6_4`）序列化；坐标 WGS84 度，ID 为有符号 64 位。`name` 优先 `name:zh`。

### 契约变化（影响 2/3/5 号）

- `include/contracts/ImportedDataset.h`：**删除** `node()` / `way()` / `relation()` / `nodes()` 及 `OsmNode`/`OsmWay`/`OsmRelation`/`OsmMember`（缓存不再保存原始对象；删除前仅导入模块自己的测试使用）。**新增** `OsmPoi`、`pois()`、`findPois()`。`roads()` 签名与语义不变（矩形内至少一个节点的整条路）。
- `include/contracts/MapGeometry.h` 接口不变；标准 OSM 的面改读 libosmium 组装结果，`AreaReadStats::skipped` 为导入时统计的跳过数。OSMX 仍由 `MapAreas.cpp` 现场拼接。
- `app::ImportStats` 新增 `pois` / `roads` / `areas` / `areasSkipped`；`nodes` / `ways` / `relations` 等仍是源文件读到的对象数。标准 OSM 的 `formatVersion` 为 2。
- **旧 schema 1 缓存不兼容**：不再出现在数据集列表，打开会报“导入缓存版本不受支持，请重新导入源文件”。旧文件不会被自动删除。
- 导入页“文件信息”增加一行“提取要素”，数据集列表对标准 OSM 显示 POI / 道路 / 面数量。

### 行为差异与限制

- 输入必须按 节点 → 路 → 关系 排序（OSM 工具的标准输出顺序）；乱序文件报错。旧实现可接受乱序。
- 面的有效性以 libosmium Assembler 为准：自相交、不闭合、成员缺失的面被跳过并计数；主题标签只放在外环成员路上的旧式 multipolygon 不会成面（libosmium 2.20.0 的组装器不处理，旧实现曾兼容）。只有 2–3 个节点的“面”不再计入跳过数。
- 既是 multipolygon 成员、自身又带面标签的闭合路，会各自成面（旧实现丢弃成员路自身的面）。
- 路/关系类 POI 的位置是外包矩形中心，可能落在凹多边形之外。
- 重复路 ID 由主键拒绝；重复关系 ID 不检查（关系不入库）。

### 验证（2026-11-11）

环境：云端容器 Ubuntu 24.04.5 amd64、Qt 6.4.2、GCC 13.3.0、CMake 3.28.3、libosmium2-dev 2.20.0-1、Release、offscreen。**不是团队统一虚拟机，不作为验收证据；统一 Ubuntu 虚拟机复验、湖北真实 PBF 导入与桌面检查均未做。**

- 根 CTest 19/19 通过（修改前基线同为 19/19）；`p1-import-tests` 19 个用例通过：五种扩展名下 POI/道路/面提取、拆成两条外环路的湖 + 内洞、建筑面 POI、缺节点/缺成员/自相交、乱序、取消与回滚、旧缓存不列出。
- `p1-pyramid-tests` 改为经真实解析器导入后渲染，面/内洞/道路像素与 POI 不上图的断言通过。
- 合成规模检查（非基准，仅确认可运行）：31 MB XML、34 万节点、800 条道路、4 万建筑、2 万 POI，`p1-import-cli` 用时约 1.5 s，输出缓存 17 MB。
- Navit 对标：`NAVIT-20261111-1-osm-feature-import-01`，受阻未测。

---

## 2026-11-01：原始 OSM 缓存（历史，已被上节取代）

2026-11-01；决策 `DEC-20261101-1-import-backend-01`。统一 Ubuntu Debug/Release 全工程 CTest 各 7/7 通过；导入专项 17/17、UI 12/12。湖北 PBF 实际导入 9,077,243 节点、593,776 道路、5,262 关系，3 类引用缺失警告。不代表 StorageAdapter 或离线包完成。

采用 libosmium 2.20.0（Boost Software License 1.0）、protozero 1.7.1 与 SQLite。支持 OSM 快照 `.osm`、`.pbf`/`.osm.pbf`、`.osm.gz`、`.osm.bz2`。不支持 history/change；不再将自定义 OSMX 统计解析作为正式入口。来源：[libosmium](https://osmcode.org/libosmium/)、[格式说明](https://osmcode.org/file-formats-manual/)。发布时需随依赖保留各自版权及许可证。

Qt 工作线程流式解析节点、道路、关系、标签与有序引用，计算源文件 SHA256。SQLite 单事务写入同目录临时缓存，提交并关闭后原子重命名；取消或错误不发布半成品。重复对象 ID、无效坐标、截断输入报错；区域裁剪造成的缺失引用保留并警告，不冒充完整路网。源文件大小和修改时间变化会拒绝发布。无 512 MiB 人为上限；可用空间、单对象大小与解析库缓冲仍限制规模。

公共契约为 `include/contracts/ImportedDataset.h`，返回独立值对象；坐标 WGS84 经纬度、单位度，ID 为有符号 64 位。消费者通过接口读取，不直接访问私有表。按 ID 查询节点/道路/关系、节点分页每次最多 1000 个；调用方仍应在后台做批量读取。该缓存保存原始 OSM，并不生成 A*/HMM 路由图、空间索引或渲染瓦片。3/5 号需要相应转换，X07/G7 未接入。

默认缓存位于 Qt AppLocalDataLocation 的 `imports/`；页面在后台读取数据集目录，重启后恢复列表。当前目录中任一损坏缓存会显示目录读取错误，需移出损坏文件后重试。页面复用 design.md 既有主题和卡片，增加日志/数据集标签页。

Ubuntu 依赖：Qt 6.4.2 Widgets/Concurrent/Test、SQLite3、Expat、zlib、bzip2、libosmium2-dev=2.20.0-1、libprotozero-dev=1.7.1-1。无管理员权限时可用 `apt-get download` 和 `dpkg-deb -x` 解到用户 SDK 前缀，然后将该前缀传入 `CMAKE_PREFIX_PATH`；不将 SDK 纳入仓库。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/g++-13 \
  -DCMAKE_PREFIX_PATH=/home/navigation/p1-deps/osmium-2.20/root/usr
cmake --build build -j1
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
build/src/data/import/p1-import-cli /path/hubei.osm.pbf /path/cache
```

测试覆盖五种扩展名、中文与原始引用保留、坏输入、取消和回滚、目录重读、导入时页面切换与主题切换。首次配置发现 Qt 6.4 延迟目标处理无法访问子目录的 imported dependency，已改为 GLOBAL 后全工程编译通过。VM 恢复后 Release 首次未指定编译器，系统无 g++ 默认别名导致失败；使用干净构建目录并显式指定 /usr/bin/g++-13 后通过，不改团队工具链。Navit 对标与原始证据见 docs/benchmarks/navit/NAVIT-20261101-1-osm-import-01.md 及同名目录。
