# OSMX 路网文件格式与字段语义（v1）

本文是 `OsmxParser` 读取的 **OSMX v1** 二进制格式契约，供 2 号（解析/存储）、3 号（A*/HMM）、5 号（LinuxRenderer）共同使用。解析实现见 `src/data/osmx/OsmxParser.cpp`，数据语义见 [OSMX 数据接口契约](osmx-api.md)，本地存储约定见 [OSMX 数据存储约定](osmx-storage.md)。

状态：2026-11-02 由 2 号实现并核对真实样例 `test.osmx`；该格式为课程统一数据格式，**不是**标准 OSM PBF/XML，也不是旧的“自定义 OSMX 统计文本”。标准 OSM 仍由 `src/data/import/`（libosmium + SQLite）导入，两条路径并存。

## 1. 通用约定

| 项目 | 约定 |
| --- | --- |
| 字节序 | 全部小端（little-endian） |
| 对齐 | 无填充、无对齐要求；定长字段后可紧跟奇数偏移的 UTF-16 字符串 |
| 整型 | `u8`/`u32`/`u64` 无符号；对象 ID 以 `u64` 存储，读出后按有符号 `int64` 使用并保留原始 OSM ID |
| 浮点 | IEEE-754 `double`（8 字节） |
| 字符串 | `u32` 字节长度 + UTF-16LE 数据；长度必须为偶数、≤ 1 MiB、不含 NUL、不得有孤立代理项；空串长度 0 |
| 坐标 | WGS84 经纬度（度），不投影、不加密偏移 |
| 距离 | 米；大地线 haversine，平均地球半径 6,371,008.8 m |
| 版本 | `u32 version = 1`；布局或语义变化必须升版本，不兼容旧版本 |

文件以 `OSMX` 魔数开始，以关系表结束，**v1 不允许尾随数据**（严格模式）。解析器拒绝 history/change 语义：文件必须是一次清洗后的快照。

## 2. 文件布局

### 2.1 头部

| 偏移（相对文件起点） | 长度 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0 | 4 | `magic` | ASCII `OSMX` |
| 4 | 4 | `version` | 当前为 `1` |
| 8 | 4 | `sourcePathBytes` | 头部来源路径的 UTF-16LE 字节数 |
| 12 | N | `sourcePath` | 生成方记录的来源文件路径，**仅作追溯**；不用于定位数据 |
| 12+N | 1 | `flags` | bit0=1 表示存在边界；其余位必须为 0 |
| 13+N | 32 | `bounds` | 仅当 `flags & 1`：`minLat, minLon, maxLat, maxLon` 四个 `double` |
| 13+N (+32) | 8 | `nodeCount` | 节点数量 `u64` |

`bounds` 必须满足 WGS84 范围且 `minLat ≤ maxLat`、`minLon ≤ maxLon`。解析器会用节点实际范围复核；不一致超过 `1e-4` 度时给出警告，并以实际节点范围为准。无边界标志时边界由节点计算。

### 2.2 节点表

`nodeCount` 之后连续存放节点记录，无分隔：

```text
node := u64 id | double lat | double lon | u32 tagCount | tag[tagCount]
tag  := u32 keyBytes | UTF-16LE key | u32 valueBytes | UTF-16LE value
```

最小记录 28 字节（无标签）。标签按原顺序保存，允许同一对象重复键（查询返回第一个匹配项）。

### 2.3 道路表

节点表结束后：

```text
u64 wayCount
way := u64 id | u32 refCount | u64 nodeId[refCount] | u32 tagCount | tag[tagCount]
```

- `refCount` 是节点引用数，允许 0/1，但少于 2 的道路无法形成线段，解析器排除出拓扑与有向图并给出警告。
- 引用必须指向本文件已出现的节点；**任一悬空引用即视为数据损坏**并拒绝整个文件（清洗后的路网不应出现）。
- 有向/无向、长度、道路属性由解析器根据标签推导，见第 3 节。

### 2.4 关系表

道路表结束后：

```text
u64 relationCount
relation := u64 id | u32 memberCount | member[memberCount] | u32 tagCount | tag[tagCount]
member   := u32 type | u64 ref | u32 roleBytes | UTF-16LE role
```

`type`：`0=node`、`1=way`、`2=relation`。成员引用允许指向裁剪范围之外的对象（路线关系常见）；解析器不拒绝文件，而是统计缺失数量并写入警告。关系成员的角色（`outer`、`inner`、`stop` 等）原样保留。

文件必须在关系表最后一个标签字节处结束；多出字节报 `trailing_data`。

## 3. 字段语义与道路属性推导

### 3.1 实体与拓扑

| 概念 | 含义 |
| --- | --- |
| 节点 | 经纬度点；可带标签（站点、入口、红绿灯等） |
| 道路 | 有序节点引用 + 标签；可表示道路、建筑轮廓、水系、铁路等 |
| 关系 | 成员 + 标签；常见 `type=route`、`type=multipolygon`、公交线路等 |
| 有向边 | 相邻节点对 + 所属道路 + 段序号 + 米制长度；双向道路生成正反两条边 |
| 无向段 | 相邻节点对一次（不论方向）；构建有向边的基础 |

### 3.2 道路分类

`highway` 标签映射为稳定枚举 `RoadClass`（`unknown` 表示缺失或未识别）：`motorway`、`motorway_link`、`trunk`、`trunk_link`、`primary`、`primary_link`、`secondary`、`secondary_link`、`tertiary`、`tertiary_link`、`unclassified`、`residential`、`living_street`、`service`、`pedestrian`、`track`、`footway`、`cycleway`、`path`、`steps`、`bridleway`、`construction`、`busway`、`bus_guideway`、`escape`、`corridor`、`platform`、`raceway`、`rail`、`ferry`。

`construction`、`raceway`、`bus_guideway`、`unknown` 不参与导航图；其余分类都可作为可通行道路候选。**是否允许某类车辆/行人通行由 3 号的算法策略决定**，解析器只提供分类、单行、限速等事实字段。

### 3.3 可导航性（`RoadRoutable`）

同时满足以下条件才置位：

1. `highway` 映射到可导航分类；
2. 不是面要素（`area=yes`、`building` 或 `landuse` 存在）；
3. `access`/`vehicle`/`motor_vehicle` 不是 `no`/`private`；
4. 节点引用数 ≥ 2，且解析后几何有效。

后续再被排除时（例如几何不足），解析器清除该标志。3 号应使用 `RoadAttributes::isRoutable()` 而不是仅比较分类。

### 3.4 单行

| 输入 | 结果 |
| --- | --- |
| `oneway=yes/true/1` | 仅沿道路节点顺序 |
| `oneway=-1/reverse` | 仅逆节点顺序；置 `RoadOnewayReversed` |
| `oneway=no/false/0` | 双向 |
| 缺失且 `junction=roundabout` | 按单行（正向） |
| 缺失且分类为 `motorway`/`motorway_link` | 按单行（正向） |

### 3.5 限速、车道、层

- `maxspeed`：支持 `50`、`50 km/h`、`30 mph`、`walk`、`none`；换算为 km/h，范围 1–300；无法解析或 `none`/`signals`/`variable` 记为 0（未知）。**0 表示未标注，不表示 0 km/h**。
- `lanes`：正整数，1–255；未知为 0。
- `layer`：整数，裁剪到 [-100, 100]；`bridge`/`tunnel` 另以标志表示。
- `bridge`/`tunnel`：任意非 `no/false/0` 的值置位。

### 3.6 显示名称

优先 `name:zh`，其次 `name`，再次 `name:en`；空值跳过。原始标签仍可通过 `wayTag()` 查询。5 号绘制地名时使用该显示名，不自行猜测。

### 3.7 长度

每条道路的 `lengthMeters` 是相邻节点 haversine 距离之和；零长度段（重复坐标、自环）跳过，不生成有向边。该长度是几何长度，不含转向代价、坡度、交通状况或等待时间。

## 4. 校验与错误模型

解析器先校验头部、计数与切片边界，再逐对象校验；任何结构错误都会返回带字节偏移的 `OsmxError`，**不发布部分数据**。主要错误码：

| 错误码 | 触发条件 |
| --- | --- |
| `open_failed` / `io_error` | 文件不存在、无权限、读取失败 |
| `file_too_large` | 超过 8 GiB 上限（防止 32 位索引/映射溢出） |
| `truncated` | 任何字段越过文件末尾 |
| `bad_magic` | 魔数不是 `OSMX` |
| `unsupported_version` | 版本不是 1 |
| `unsupported_flags` | `flags` 含未知位 |
| `bad_count` | 数量与剩余字节不可能匹配，或超过 32 位索引上限 |
| `bad_string` | 奇数长度、超过 1 MiB、NUL、非法 UTF-16 代理项、空标签键 |
| `bad_coordinate` | 非有限值或超出 WGS84 范围 |
| `bad_bounds` | 头部边界非法 |
| `duplicate_id` | 节点/道路/关系 ID 重复 |
| `corrupt_record` | ID 为 0、道路悬空节点引用、关系成员类型非法 |
| `trailing_data` | 关系表后仍有字节 |
| `file_changed` | 加载期间源文件大小/修改时间变化 |
| `cancelled` | 调用方取消 |
| `invalid_dataset` | 构建内存索引时发现不一致 |

限制：

- 节点、道路、关系数量均须 < 2³²；字符串池与标签总数同样受 32 位索引限制。
- 单个字符串 ≤ 1 MiB；整个文件 ≤ 8 GiB。
- 解析器不做坐标裁剪、坐标系转换或 protobuf 解码。

## 5. 真实样例 `test.osmx`

2026-11-02 核对结果（SHA-256 为文件内容）：

| 项目 | 值 |
| --- | --- |
| 文件大小 | 49,356,479 字节（47.07 MiB） |
| SHA-256 | `a9c4d90b049a5e34ed37c3dfecb7d76a6ff96425c1da7032536199fb2ef16467` |
| 头部来源路径 | `C:/Users/hzh/Downloads/map (2).osmx`（仅追溯） |
| 边界 | 29.833451, 111.2543204 — 30.9806443, 114.8508807（WGS84） |
| 节点 | 912,730（标签 60,439） |
| 道路 | 121,964（标签 260,850；节点引用 1,148,144） |
| 关系 | 1,658（标签 9,145；成员 77,336） |
| 可导航道路 | 44,986 |
| 无向段（含建筑/水系等全部道路） | 1,026,180 |
| 有向边 | 536,688 |
| 可导航道路几何长度 | 16,408.873 km |
| 数据警告 | 关系成员引用缺失：节点 584、道路 60,794、关系 790；道路表节点引用完整，无悬空引用 |

缺失的关系成员来自裁剪到武汉/湖北范围后保留的完整路线关系，属于已知限制；道路表的节点引用完整，不影响 A*/HMM 的图构建。该文件不是本仓库的 Git 资产，验收时由 2 号/1 号随离线数据包提供，并以 SHA-256 核对。

## 6. 版本演进与兼容

- 任何字段顺序、宽度、语义变化都必须升 `version`；解析器对未知版本报错，不尝试猜测。
- 新增可选语义优先使用新标签或保留字段，不改变既有字段含义。
- 缓存清单记录 `formatVersion`；升级后旧数据集仍可按 version 1 读取，或在重新导入时生成新缓存目录。
- 生成方（清洗工具）与读取方（本仓库）以本文为唯一格式依据；生产新的 `.osmx` 前先更新本文并补充样例/测试。
