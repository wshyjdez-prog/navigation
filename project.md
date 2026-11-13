# P1 Linux 导航应用：总体规划与实现状态

最后更新：2026-11-11（Asia/Shanghai）。组员：1、2、3、4、5。

当前面名称增量（2026-11-11，1 号；影响 5 号渲染）：地图面图层显示名称，随 XYZ 金字塔分级出现。名称在建索引时取面内最远点，严格避让后写入瓦片，跨瓦片无缝。按 `DEC-20261111-1-area-labels-01` 对 A 瓦片内烘焙和 B 帧级叠加做了实测，组员 1 最终选 A，B 已清理。统一基线虚拟机根 CTest 19/19；湖北真实数据下热帧约 2–20 ms，索引因名称增加约 1.6 s。Navit 渲染对标受阻未测（`NAVIT-20261111-1-area-labels-01`），整体保持进行中。同日修正道路名：道路名并入同一套严格避让，相距 < 256 px 的同名标注只显示一个。同日修复拖动/缩放时的边缘空白：按 `DEC-20261111-1-edge-loading-01` 比较了 A 外扩帧+渐进+粗底图与 B 后台预取环，组员 1 选 A，B 已清理（见下方边缘加载记录）。同日新增地图菜单“地图清晰度 高/中/低”（瓦片密度 2×/1.5×/1×，默认低，见下方清晰度记录）。同日新增“路线模式：一般/行车”，行车模式按 Navit 0.5.7 car 配置排除人行道等非机动车道路（`DEC-20261111-1-route-mode-01`，影响 3/4 号）。同日起跨城远距离路线改用仿 Navit 的分级路网（`DEC-20261111-1-long-route-01`，本次忽略对比）。同日新增兴趣点搜索（右上角按钮、最近 20 个、点击看信息、右键“到这去”）与起终点吸附虚线；POI 名称索引按 `DEC-20261111-1-poi-index-01` 比较三种组合后选 B 内存单字倒排（影响 2 号）。同日补充：导入后对同名且相距 ≤ 100 m 的 POI 合并为一点（`DEC-20261111-1-poi-dedup-01`，湖北合并 3,023 个，旧缓存在搜索时套用同一规则），结果改为紫色定位针并显示名称前 5 个字。旧金字塔缓存升级为 `xyz-v4-labels2` / `raster-style5-labels`，会自动重建。详见下方 2026-11-11 面名称记录。

当前导入增量（2026-11-11，1 号授权跨模块修改，主责 2 号）：标准 OSM 导入改为 libosmium 要素导入，导入时直接提取 **POI、道路、面** 写入 schema 2 缓存（`pois` / `roads` / `areas`），面的外环/内洞由 `osmium::area::Assembler` 组装，取代标准 OSM 路径上的手写拼接；**POI 只入库、可按矩形/类别/名称查询，不在地图上显示**。`ImportedDataset` 删除原始节点/路/关系读取，新增 `pois()` / `findPois()`，`roads()` 与 `MapGeometry.h` 接口不变；OSMX 入口不变。旧 schema 1 缓存需重新导入。云端容器（Ubuntu 24.04.5 / Qt 6.4.2 / GCC 13.3.0，非统一虚拟机）根 CTest 19/19 通过；**统一虚拟机复验、湖北真实 PBF 与桌面检查未做，Navit 对标受阻未测**，状态保持进行中。详见 [导入说明](docs/osm-import.md)、`DEC-20261111-1-osm-feature-import-01`、`NAVIT-20261111-1-osm-feature-import-01`、AI 日志 18。下文“标准OSM缓存”的面拼接描述以本段为准。

当前面要素增量：`MapGeometry.h` / `MapAreas.cpp` 提供标准OSM缓存与OSMX的闭合面、multipolygon外环/内洞流，供 `TilePyramid` 写入独立面RTree后绘制水域、绿地、土地利用、建筑；沿用既有异步执行、XYZ及LRU方案，派生缓存升级为 `xyz-v2-areas1` / `raster-style3`，不改路由图与源缓存。1号授权装配，2号维护数据契约、5号维护绘制。Windows Qt6.4.2预检已通过面拼接、内洞、道路覆盖、取消及主题像素测试；统一Ubuntu Release构建、根CTest 19/19（13.73 s）及启动检查通过，新版已安装并打开于 install-map-areas1；全省实际面数据视觉复核与同源Navit性能补测尚未完成，整体保持进行中。未支持海岸线开放链构面、嵌套relation成员和任意自交多边形修复；缺失/不闭合引用跳过，派生metadata保存 emitted/skipped 数量。用户要求本次不新增日志记录。

## 1. 组名、任务依据与当前事实

**组名：Linux 端导航应用开发组（Linux PAL 适配）；简称 P1 Linux 组。**

当前依据为用户提供的《P1 Linux组.pdf》（4 页，页眉显示导出时间 2026/10/17 11:29）。正文摘录见 [Linux 任务书](docs/linux-assignment-reference.md)。该 PDF 替代旧鸿蒙任务；[旧任务书摘录](docs/assignment-reference.md) 仅用于历史追溯，不再作为当前 P1 验收要求。本文的五人分工、目录和内部契约是团队实施规划，不代表其他组已确认接口。

目标：使用 C++、Qt Widgets / OpenGL ES 构建完整 Linux 导航应用，集成 A* 路径规划和 HMM 地图匹配，完成 Linux PAL（平台抽象层）的渲染、语音、存储、定位四项适配，形成“导入 → 计算 → 定位导航 → 地图/语音 → 离线使用”的闭环，并与 G6 联调。

PDF 总任务中的“适配四大平台”未列出平台清单；本组明确交付范围为 Linux 可运行应用和四项 PAL 适配，公共接口须支持跨平台复用。是否还要求产出其他平台实现需向教师核实，不把“四项适配”直接等同“四个平台”，也不擅自增加其他平台交付。

已完成：团队规划、五人分工、跨组接口登记、协作规则、UI 设计基准与方案抉择模板。1 号已建立第一次作业 Qt Widgets/CMake 应用框架源码：窗口、页面导航、明暗主题、公共组件演示、文件选择占位与模块页面注册；构建与桌面验收结果见下方进度和 [框架说明](docs/assignment-1-framework.md)。标准 OSM 原始数据解析/缓存已接入；算法、导航及完整四项 PAL 业务仍待集成。原 Ubuntu 虚拟机曾出现根文件系统 emergency_ro；用户已选择保留现状并从基础快照独立克隆验证，不能沿用旧环境成功记录证明本次程序可运行。

## 2. 编程环境与工程启动

| 项目 | 当前任务要求/统一约定 | 实测状态与待办 |
| --- | --- | --- |
| 目标系统 | Linux 桌面虚拟机：Ubuntu Desktop 24.04 LTS（noble），x86_64/amd64 | 1 号已验证 Ubuntu 24.04.4 amd64 / Wayland / 官方 ISO SHA256；4 号已在统一虚拟机复现（Ubuntu 24.04.4 LTS amd64，见 [member-4 记录](docs/environment/member-4-2026-10-28.md)）；2/3/5 号待复现 |
| 语言与界面 | C++17、Qt 6.4.2 / Qt Widgets；地图按 Qt Widget 或 OpenGL ES 适配 | 1 号 Qt 6.4.2 / C++17 / Qt Widgets 窗口通过；业务渲染后端待选择，OpenGL ES 未验证 |
| IDE/构建 | Qt Creator；GCC/G++ 13.3.0；CMake 3.28.3；完整 CMake 工程 | 1 号 GCC/G++ 13.3.0、CMake 3.28.3 与 Qt Creator 专用 Kit 已验证；独立检查工程构建通过；4 号 GCC/G++ 13.3.0、CMake 3.28.3、Ninja 1.11.1 已核对，构建与 ctest 通过 |
| 渲染 PAL | LinuxRenderer；瓦片、路线、标记及异步绘制 | 绘制接口、上下文/线程约束由 5 号落实；OpenGL 路线需验证上下文与驱动 |
| 语音 PAL | LinuxVoiceAdapter；本地 TTS 与本地 ASR、中文环境、FFI 回调 | 后端/模型/依赖、回调 ABI、所有权及生命周期由 1 号核实 |
| 存储 PAL | StorageAdapter；SQLite 与本地文件系统 | schema、SQLite 接入、事务/路径与离线包格式由 2 号确认 |
| 定位 PAL | LocationAdapter；GeoClue2 | 服务与权限/应用标识配置、回调与拒绝访问处理由 4 号验证 |
| 权限与分发 | 声明 LOCATION/MICROPHONE 权限；离线数据包随应用分发 | 1 号汇总分发配置，4 号负责定位、1 号负责麦克风；按目标发行版/打包方式确认具体声明与授权机制，不能把两个名称当作通用 Linux 配置键 |
| 版本控制 | Git / GitHub；UTF-8 文档 | origin 为 https://github.com/wshyjdez-prog/navigation.git |

完整基线、安装/核对命令、共享镜像/快照和五人验证状态见 [统一开发环境](docs/development-environment.md)。1 号本机安装与运行证据见 [实测记录](docs/environment/member-1-2026-10-26.md)，其他成员尚待复现。

1 号先交付最小 Qt Widgets 工程和 Linux 运行证据，记录精确环境版本、从全新克隆到配置/构建/运行的实测命令、运行依赖及离线资源位置，由另一位组员复现。其他成员可先依据公共契约设计样例/Mock；工程创建前不提供虚构的可用构建命令。虚拟机或其他辅助环境须如实标注，最终证据需满足 Linux 运行要求。

## 3. 五人分工与独立验收

保留原先的数据、算法、导航、地图归属以减少交接；原有分布式协同任务移除后，由 1 号承接语音 PAL，使第 2 次作业的渲染与算法集成能并行，第 3 次作业的语音、存储、定位也能分别推进。各人负责自己模块的代码、业务页面、样例、测试和技术文档，1 号只汇总集成与发布。

| 编号 | 主责与边界 | 独立开发方式 | 验收与交付 |
| --- | --- | --- | --- |
| 1 | Qt 应用框架、窗口/页面/主布局/主题、公共 UI 与 CMake 总入口；LinuxVoiceAdapter；用户功能页面；打包发布 | 使用 Mock 导航事件实现窗口与列表/卡片/对话框/表单/进度条；独立测试 speak()/listen() | Linux 可执行程序；中文本地 TTS/ASR；语音回调经 FFI 返回核心层、生命周期/停止/异常处理；麦克风权限；用户页面通过 2 的接口访问本地数据；安装运行说明和离线包随程序分发 |
| 2 | 文件选择/本地文件管理/解析、导入进度/统计/列表；StorageAdapter、SQLite、本地用户数据与离线包 | 固定路网/POI/轨迹样例与独立临时数据库；模块自己的导入/包管理页面 | 正常/错误/取消导入；存储读写与事务；收藏/历史/偏好本地持久化；离线包下载/存储/更新/恢复；提供随应用分发的数据包和清单 |
| 3 | C++ A*/HMM 集成、统一算法接口、数据转换、后台任务；算法调用页和性能监控 | 用固定图和轨迹验证算法，Mock 供其他成员开发；算法页通过 5 的接口显示结果 | 界面真实调用 C++ A*/HMM；路径/匹配结果正确；耗时/内存/线程状态可观察；算法不阻塞 UI；处理失败、取消、资源释放；维护模块级 CMake 目标 |
| 4 | 导航状态机、LocationAdapter/GeoClue2、定位权限、偏航检测/重算、模拟定位与暂停/加速/回放；G6 联调牵头 | 用事件与固定轨迹驱动状态机，使用 3 的 Mock；不依赖真实位置和地图也能验证转换 | idle/规划中/导航中/偏航/到达；位置回调、拒绝权限/服务不可用处理；模拟与真实源切换；偏航重算和到达；与 G6 联调测试报告（其他成员提供各自接口证据） |
| 5 | LinuxRenderer、Qt Widget/OpenGL ES 地图绘制、瓦片/路线/标记、交互、仪表盘及引导显示 | 独立渲染演示页消费固定 MapScene 和 NavigationSnapshot | drawTile→drawImage、drawRoute→drawLine、present→flush 流程；渲染不阻塞 UI；异步与上下文/线程管理；速度/剩余距离/转向提示；绘制技术文档与性能证据 |

交接边界：2 号只负责数据存取，1 号负责用户页面及语音；3 号计算结果，4 号控制导航/重算时机；5 号消费快照展示，1 号播报 4 号产生的引导并将语音控制事件回传核心层；4 号处理事件。1 号统一顶层 CMake/窗口注册，其他人维护模块目标，避免多人争改入口。

### 四项 PAL 的唯一主责

| PAL 模块 | 主责 | 消费方/配合方 |
| --- | --- | --- |
| LinuxRenderer | 5 | 3 算法结果、4 导航快照、2 瓦片数据、1 应用入口 |
| LinuxVoiceAdapter | 1 | 4 引导/控制事件，G6 核心回调边界；G10 或本地语音后端 |
| StorageAdapter | 2 | 1 用户页面、3 算法数据、5 地图数据 |
| LocationAdapter | 4 | 导航核心/G6，1 汇总分发和权限说明 |

## 4. 并行架构与契约草案

### UI 设计基准

地图加载过渡当前修复：`src/ui/map/MapFrame.h` 按地理范围平移/缩放上一张完整底图，供显示线程之外的过渡合成使用；`MapView.cpp` 已移除首次局部路网的单独绘制，`TilePyramid.cpp` 的部分瓦片帧保留已有底图，并用就绪瓦片逐块替换，避免初始武汉区域固定残留和已加载区域闪空。画布在交互事件内立即变换已显示帧，并完整不透明重绘；父级临时瓦片只补未覆盖部分，不压掉已有细节。路线、标记仍按当前视角重画。Windows Qt 6.4.2 过渡/边框/金字塔测试 6/6 与部件隔离测试 7/7 通过（含后台帧未返回前的缩放/拖动像素回归）；三项修复合并后，统一 Ubuntu 增量构建与根 CTest 19/19（13.73 秒）通过，已部署至 `/home/navigation/p1-pyramid-20261103-174714/install-map-flicker-fix/bin/p1-navigation`，启动检查通过，桌面快捷方式已更新。Navit 场景性能仍未测。本次不新增 AI/决策日志。

路网边框修正：`src/pal/linux/renderer/RoadPainter.h` 统一道路外描边、内部和文字的绘制顺序，用于金字塔瓦片，消除相交/分段道路内部露出的端帽边框。PNG 使用 `raster-style2` 自动隔离旧缓存，几何索引复用。Windows Qt 6.4.2 边框/金字塔回归 5/5、地图部件隔离回归 6/6 通过；本次合并修正在统一 Ubuntu 下完整回归 19/19（13.73 秒）通过，已安装到 `/home/navigation/p1-pyramid-20261103-174714/install-map-flicker-fix/bin/p1-navigation` 并通过启动检查，桌面快捷方式已更新。按用户要求不新增 AI/决策日志。

当前新增地图金字塔能力：`src/ui/map/TilePyramid.*` 提供 256 像素 Web Mercator XYZ 多级底图、后台可见瓦片与受限总览预生成、父级补位、128 MiB 内存 LRU 与每数据/样式命名空间 2 GiB PNG 磁盘缓存。`include/contracts/MapGeometry.h`、`src/data/import/MapGeometry.cpp` 由数据侧流式提供几何，派生只读 RTree 索引不影响原始缓存和路由图；OSMX 使用整个已加载文件，标准 OSM 使用整个已导入缓存。默认显示 0–18 级，道路类别与绘制点密度随层级变化。1 号集成，影响 2 号地图几何供数与 5 号渲染。用户明确仅本次忽略多方案对比，采用 C；本次不新增 AI/决策/性能日志条目。源码与 Windows Qt 6.4.2 编译预检、金字塔测试 4/4 和地图部件隔离测试 6/6 通过，Ubuntu 24.04 / GCC 13.3.0 / Qt 6.4.2 下增量构建与根 CTest 19/19（13.33 秒）通过，新版已安装于 `/home/navigation/p1-pyramid-20261103-174714/install/bin/p1-navigation`，启动检查通过、桌面快捷方式已更新。首次打开已有数据集时后台建立派生索引与瓦片；完整湖北数据首次构建耗时及新场景 Navit 性能仍未测，模块整体验收保持进行中。

所有 UI 设计、实现与评审统一使用根目录 [design.md](design.md) v1.1：应用控件沿用 Clay 明暗主题；地图默认只显示画布与浮动菜单，右键设起终点，路网使用独立灰色系，规划路线使用蓝色。详细表单与监控位于算法页。1 号维护外壳、主题与本次集成；涉及 5 号地图交互/标记和 2 号区域读取，沿用公共只读契约、既有缓存渲染和区域查询方式，不新增外部地图接口。

地图浏览当前源码：`src/ui/map/MapView.cpp`、`MapRegionLoader.*`；拖动/缩放后后台加载已导入标准 OSM 的当前区域，过滤过时请求，保持视角与端点；空区域有提示，OSMX 限文件覆盖范围。`NavigationWorkflow` 共享右键端点，端点超出当前规划区域时通过原数据契约重建区域路网；沿用既有范围/规模限制，不提供跨省全量路由或在线下载。浏览不取消算法任务，Qt 部件只在主线程操作。Windows/Qt 6.4.2 源码编译预检、合成 SQLite 区域测试 7/7 与地图部件隔离测试 6/6 通过；部件测试未调用路线算法或标准 OSM 解析器，不能替代统一 Ubuntu 桌面、完整路由回归及新场景 Navit 对标，模块保持“进行中”。按用户要求，本次不新增 AI、决策或性能报告记录。

工程结构（应用、公共 UI、测试与根 CMake 已创建；`src/algorithms/` 已有 3 号 P0 私有模型、校验和独立测试；`include/contracts/` 已有 4 号位置源契约与导航状态契约；`src/navigation/` 已有位置源回放、导航契约测试与模块 CMake；`tests/fixtures/location/` 已有固定轨迹与五类模拟样例及逐帧期望值；`docs/navigation-contracts.md` 记录契约字段表与转换表。位置源部分已随 1 号 PR #3 进入 main，导航状态契约部分由 4 号本次以增量形式提交、待评审合并；各自的 Linux 虚拟机验证见第 7 节，合并后的完整版本仍建议统一复验；其他模块目录仍为规划位置）：

```text
CMakeLists.txt               # 1：顶层工程与目标装配
src/app/                    # 1：Qt 窗口、导航、用户页与依赖注入
src/ui/common/              # 1：通用控件
include/contracts/          # 1 维护，各模块负责人共同设计
src/data/                   # 2：导入、仓储、离线包及页面
src/algorithms/             # 3：A*/HMM 集成、工作任务及调用/性能页
src/navigation/             # 4：状态机、偏航、模拟和回放
src/ui/map/                 # 5：地图交互、仪表盘、转向展示
src/pal/linux/voice/        # 1：LinuxVoiceAdapter
src/pal/linux/storage/      # 2：StorageAdapter
src/pal/linux/location/     # 4：LocationAdapter
src/pal/linux/renderer/     # 5：LinuxRenderer
tests/fixtures/             # 各模块确定性样例
resources/offline/          # 2 提供资源，1 负责分发配置
docs/                       # 各模块说明及 G6 联调报告
```

| 公共契约 | 提供者 → 使用者 | 最小约定 |
| --- | --- | --- |
| DataRepository / StorageAdapter | 2 → 1/3/5 | 数据集 ID/版本、导入进度、图/轨迹/瓦片读取；用户本地 CRUD；包清单/下载/更新；禁止其他模块直接操作其数据库内部表 |
| RoutePlanner / MapMatcher | 3 → 4/算法页 | 异步请求 ID、输入/结果、取消/失败；路径几何/路段/距离/时间、匹配点/道路/质量信息 |
| LocationAdapter / LocationSource | 4 → 导航核心 | GeoClue2 和模拟源可替换，位置/时间/速度/方向/精度，启停/错误/权限状态。已落地契约：`include/contracts/LocationTypes.h`、`LocationSource.h`（2026-10-28，4 号分支已记录 Linux 构建通过，本次集成版本待复验）；Mock 实现 `src/navigation/MockLocationSource.*`，真实源替换点 `src/pal/linux/location/` |
| NavigationController / NavigationSnapshot | 4 → 1/5 | 导航命令、状态、路线、位置、剩余距离/时间、引导指令及序号；经 G6 确认事件映射。已落地契约：`include/contracts/NavigationTypes.h`、`NavigationController.h`（2026-10-28，Linux 构建与 ctest 已通过；**状态机实现属第 3 次作业，本次只定契约与转换表**）。Paused 为 Navigating/OffRoute 的子标志而非独立状态；偏航策略已确认为"偏离 > 15 米且持续 ≥ 3 秒，重算最小间隔 10 秒、在途不重复请求"。说明见 `docs/navigation-contracts.md` |
| LinuxRenderer / MapScene | 5 → UI；2/3/4 提供输入 | drawTile→drawImage、drawRoute→drawLine、present→flush；图像/路线/标记、选点/视角事件、异步提交与渲染完成；后端映射及函数签名待实现验证 |
| LinuxVoiceAdapter | 1 ↔ 核心层/4 | speak() 本地中文 TTS、listen() 本地 ASR；语音回调通过 FFI 传回核心层；明确 ABI、回调线程、缓冲区归属、注销与销毁顺序 |

FFI 是 PDF 明确要求，不能因主体为 C++ 就静默省略；1、4 号与核心提供方确认实际语言/ABI 边界和回调协议。OpenGL 路线下必须明确上下文归属和线程切换；采用 Qt Widget 路线时记录其绘制/flush 映射与适用限制，不能伪造 OpenGL 验证。

通用草案：ID 表示与外部接口协商；经纬度带坐标系标记，固定样例默认 WGS84；距离米、时长秒、速度米/秒、角度度、时间戳 Unix 毫秒。带 requestId、数据版本、mock/real 来源；旧任务结果不得覆盖当前导航。统一接口、错误模型、资源归属后各人用相同契约和固定样例独立开发，公共签名变更标注所有受影响模块。

## 5. 跨组接口与待确认事项

目前没有已验证的外部交付。G1/G6/G7/G10 在新 PDF 明确出现；G4/G5、G2/G3、G8 的关系沿用旧课程分组背景，属于待确认的算法来源或条件依赖，不等同新 PDF 新增的硬性接入项。

| ID | 外部组 / 本组负责人 | 接口与验收要求 | 待选择/确认 |
| --- | --- | --- | --- |
| X01 | G1 / 2 | 导入格式、字段、坐标系、版本、样例、异常 | 接 G1 或按 PDF 自行实现简单解析；固定样例仅用于并行开发 |
| X02 | G2（条件依赖）/ 2，3 配合 | 有向路网、ID、单行道、权重/限制与序列化 | 核实算法库是否自带图模型；需直接依赖才单独对接 |
| X03 | G3（条件依赖）/ 3，2 配合 | 候选路段查询、空间索引、生命周期 | 随算法库复用或单独接入；未经确认不新增强制依赖 |
| X04 | G4（沿用的算法来源候选）/ 3 | A* C++ 源码/头文件、图输入、路径输出、线程安全与 CMake 集成 | 真实接入或确认允许的自实现方案；C++ 本地集成不再使用 NAPI |
| X05 | G5（沿用的算法来源候选）/ 3 | HMM C++ 输入/参数、输出、依赖、线程安全 | 核实真实库及来源；最近邻/Mock 不替代 HMM 完成证据 |
| X06 | G6 / 4 牵头，全员配合 | 状态/事件、偏航重算、引导、PAL 接口、异常与线程/回调契约 | 第 4 次必须完成真实联调并输出报告；临时本地状态机/Mock 不能替代该报告。记录双方版本、环境、样例、预期/实际、缺陷/复测结论 |
| X07 | G7 / 2，1 配合 | SQLite schema、离线包清单/版本/校验、下载/更新恢复、随应用分发 | 按 G7 方案实现 Linux 适配；待交付时先做本地样例，替代方案不能自动等价于指定要求 |
| X08 | G8（条件依赖）/ 5 | 如复用其数据/渲染协议，确认瓦片/路线/标记、坐标与 PAL 接口 | 新 PDF 要求本组 LinuxRenderer，可选 Qt Widget/OpenGL ES；是否复用 G8 待确认，不能以外部桌面程序代替本组适配 |
| X09 | G10 / 1 | 第 4 次语音完善方案；本地 TTS/ASR、中文、FFI 回调与生命周期 | PDF 允许 G10 方案或本地 TTS；PDF 未提供 G10 详情，需核实联系人/接口，不能断言该组不存在；仅 TTS 不满足第 3 次 listen()/ASR 要求 |

已确认：Linux 桌面虚拟机及 Ubuntu 24.04 LTS、Qt 6.4.2、GCC/G++ 13.3.0、CMake 3.28.3 的团队基线；1 号本机基础环境已验证，其他成员待复现。待确认清单：Qt Widget 或 OpenGL ES 渲染路线；语音后端和 FFI 边界；权限/打包方案；性能具体阈值；“四大平台”的平台清单。此轮是需求与分工更新，没有替各负责人选定技术方案。

| 日期 | 范围 | 状态/决定 |
| --- | --- | --- |
| 2026-10-18 | X01–X09、Linux 环境与 PAL | 依据新 PDF 更新职责和约束；具体外部接口、实现版本与选型均未确认 |
| 2026-10-26 | 开发环境 | 用户确认 Linux 桌面虚拟机；团队统一 Ubuntu Desktop 24.04 LTS amd64 / Qt 6.4.2 / GCC-G++ 13.3.0 / CMake 3.28.3；外部接口与 PAL 后端仍待确认 |

关键算法方案的候选组合、2–3 种试验、实测指标、最终确认及清理记录仍使用 [decision_log.md](decision_log.md)；该文件当前只有模板。项目当前方案与模块进度需引用正式抉择 ID，不得把本次平台调整当成已测出的算法选型结果。

## 6. 四次作业与验收覆盖

| 作业/时间 | 1 号 | 2 号 | 3 号 | 4 号 | 5 号 | 集体验收与交付 |
| --- | --- | --- | --- | --- | --- | --- |
| 第 1 次 / 第 4 周后 | Linux/Qt/CMake、窗口/页面/布局/主题、公共组件、可执行程序 | 文件选择→本地管理→解析进度→统计→列表 | 图/轨迹契约、算法 Mock 和集成准备 | 状态/位置事件契约、模拟样例 | 地图演示控件/固定场景 | 可编译运行的 C++/Qt 源码与 Linux 程序、基础框架设计、数据导入演示 |
| 第 2 次 / 第 7 周后 | 集成算法页/渲染入口、准备语音适配边界 | 路网/轨迹/瓦片样例和供数 | 真实 A*/HMM 集成、起终点规划与轨迹匹配调用页、耗时/内存/线程监控 | 对接真实计算和导航事件 | LinuxRenderer：瓦片/路线/标记、三段绘制映射、上下文与异步 | 核心算法+LinuxRenderer 源码、更新后的 Linux 程序、渲染适配技术文档及算法/渲染演示；渲染和计算不阻塞 UI |
| 第 3 次 / 第 10 周后 | LinuxVoiceAdapter：本地中文 TTS/ASR、speak/listen、FFI 回调、生命周期和麦克风权限；资源分发装配 | StorageAdapter：SQLite/本地文件、离线资源随应用提供 | 支持连续匹配/重算、后台计算错误恢复 | LocationAdapter/GeoClue2、定位权限；状态机/偏航/到达；暂停/加速/回放；G6 预联调 | Qt/OpenGL 地图、速度/剩余距离/转向和引导展示 | 三个适配器源码、导航引擎源码、更新程序、Linux 设计文档、模拟导航演示；权限拒绝/语音回调/定位失败可验证 |
| 第 4 次 / 第 12 周后 | 最终集成/构建分发、语音完善、用户页面、启动性能、README/手册汇总 | G7 离线包下载/存储/更新，收藏/历史/偏好本地存储 | 算法资源/性能优化与联调支持 | 牵头完成 G6 联调与报告，导航全流程回归 | 渲染性能/内存与 UI 完善，Linux 截图/视频录制配合 | Linux 可执行程序、全部源码+CMakeLists.txt/完整 CMake 工程、用户手册/设计/API/README、G6 联调测试报告、Linux 运行截图、3–5 分钟 Linux 运行视频、完整 AI 日志 |

评分重点（来自 PDF）：第 1 次环境 20%、框架 25%、导入 25%、UI 20%、文档演示 10%；第 2 次渲染 30%、算法集成 20%、不阻塞 UI 20%、算法调用界面 20%、文档演示 10%；第 3 次状态机 20%、语音 15%、存储/定位 15%、UI 20%、偏航重算 15%、文档演示 15%；第 4 次功能 25%、四项 PAL 15%、离线与语音 15%、性能 10%、代码文档 15%、答辩核心函数 20%。

每个人验证自己的模块；全员补齐端到端、权限拒绝、服务不可用、计算失败、离线包缺失/更新失败、回调释放和非阻塞场景。第 4 次 G6 报告须有真实联调证据，尚未发生时状态写“待联调”。性能记录同一 Linux 环境的启动耗时、帧率、内存和计算耗时；新 PDF 未给具体数值阈值，应先确认并建基线，旧版“启动 <2 秒”不再作为当前硬性指标。

### Navit 持续性能对标

自 2026-10-31 起，团队将 **Navit** 作为性能对标项目，后续五位成员新增、完善或优化的功能模块均须比较等价任务并记录。按 [AGENTS.md 第 8 节](AGENTS.md#8-以-navit-为持续性能对标项目) 和 [对标规范、报告模板及索引](docs/navit-benchmark.md) 执行：1 号维护共同 Navit 版本/配置基线，各模块负责人在同一统一 Linux 虚拟机、同源数据与可比场景下实测，保存本组/Navit 数值、单位、差异、质量限制、原始数据及复现步骤。具体报告归档到 `docs/benchmarks/navit/`，模块完成记录引用报告 ID；算法抉择同时关联 `decision_log.md`。

目前只确认了对标要求，**Navit 具体版本、配置、数据和运行基线尚未建立，没有实测比较或性能结论**。缺少条件时记录受阻/未测、责任人与补测步骤；无对应能力时核查并说明不适用及仍可比的子任务。可比项未测不能把模块整体验收标为已完成，功能实现与对标状态分别记录；已有历史验收不追写虚构指标，后续修改或阶段验收时补齐。一次忽略算法多方案试验不豁免本要求，比较落后也如实留存，不自动设定“必须快于 Navit”的门槛。

新 PDF 不再要求鸿蒙分布式协同、账号同步或 HAP；用户数据明确本地存储。答辩 PPT 可另行准备，但不能把旧任务的页数要求当作新 PDF 已明确的交付项。周次未给起始日期，不换算日历截止日。

## 提示词框架与任务启动

入口：[prompts/README.md](prompts/README.md)。通用开场读取项目事实，按 AGENTS.md 第 1.1 节和已确认编号采用专业角色，组员模板定义本人职责与工程检查重点，工作流程模板覆盖工程初始化、模块开发、算法组合对比、跨组/G6 联调、排错、验收和交接。每次填写目标、范围、验收与已确认事项；提示词中的例子和规划路径不代表已有实现。

## 7. 实现进度与变更记录

| 模块 | 主责 | 状态 | 证据/下一步 |
| --- | --- | --- | --- |
| Linux 任务规划与分工 | 团队公共 | 已完成（文档） | project.md、README、AGENTS.md、任务摘录及 decision_log.md 已更新 |
| 提示词框架 | 团队公共 | 已完成（文档） | prompts/ 的通用开场、五人职责与七类流程；业务状态不变 |
| Navit 性能对标基线与模块比较 | 全员，1 汇总 | 未开始（实测） | 对标规则/模板已建立；Navit 版本/配置、同源数据和运行基线待建立，各模块负责人按 docs/navit-benchmark.md 补齐比较报告 |
| Linux 基础开发环境 | 1 | 进行中 | 历史基础验证通过；原机 emergency_ro 未修复，用户授权克隆的版本/应用自动验证及常规原生桌面补测通过；其他成员复现仍待完成 |
| UI 设计规范 | 1，2/5 协作 | 已完成（文档） | design.md v1.0；主题/公共组件已有实现与 offscreen 截图；完整桌面、地图/导入业务验收待完成 |
| Qt 工程/框架/CMake | 1 | 进行中 | 真实源码与更新 Linux 包已产出；offscreen/Wayland 测试各 8/0，常规桌面与文件选择已补测；高 DPI/200% 字号和完整人工键盘验收仍待进行 |
| 第二次作业应用集成 | 1 汇总，2/3/4/5 协作 | 进行中 | 五人源码已共同装配，Ubuntu CTest 17/17、Wayland UI 13/13、OSMX 页面4/4；真实 A* 桥与 FMM 调用层进入根构建。算法调用页/监控和生产数据转换未贯通；见 docs/assignment-2-team-integration.md |
| 窗口/导航/主题/公共组件基线 | 1 | 已完成（基础子项） | 真实 Wayland 桌面启动、屏幕适配、主题/组件与文件选择回传通过；源码、双平台测试、实际桌面截图与安装入口见框架说明；不包含真实导入和完整缩放验收 |
| 数据导入/StorageAdapter/离线包 | 2（1 号整合） | 进行中 | 标准 OSM/PBF 与 OSMX 均进入根工程，Ubuntu 导入/缓存/重开测试通过；真实 OSMX 样例本次缺失跳过，完整存储及 G7/X07 离线包仍待办。**2026-11-11**：标准 OSM 改为 libosmium 提取 POI/道路/面（schema 2，POI 不上图），云端容器根 CTest 19/19；统一虚拟机复验、真实 PBF、Navit 对标（NAVIT-20261111-1-osm-feature-import-01）未完成，旧缓存需重新导入，见 docs/osm-import.md |
| C++ A*/HMM/性能页 | 3（1/4 号整合） | 进行中 | 算法源码已合并，真实 A* 与4号桥共建回归通过；FMM 适配层已编译/调度测试，当前 VM 真实 FMM 未复验。算法页/监控/生产转换仍待完成 |
| LocationAdapter/导航/G6 联调 | 4 | 进行中 | 位置源契约、固定轨迹读取与 Mock 回放已完成；main（`9cc04b1`）Linux 复验通过（ctest 7/7）。**第二次作业对接层已交付（2026-11-01）**：`AsyncPlannerBridge`（QThread+Worker 线程化对接，落实契约"计算不阻塞 UI"）、`StubRoutePlanner`（确定性桩，延迟/失败/取消可注入）、`RecordingController`（验证桩）、`p1-planner-demo` 与 7 例集成测试；虚拟机实测 configure/build 零警告、全量 ctest **9/9**（新增 navigation-integration、navigation-planner-demo）。接口选择见 DEC-20261101-4-planner-bridge-interface-01（契约先行，3 号真实 A*/HMM 实现既定 `RoutePlanner` 接口即插即换）、DEC-20261101-4-async-model-01（本次忽略）。Navit 对标受阻未测（NAVIT-20261101-4-planner-bridge-01）。导航状态机、偏航/重算去重、GeoClue2、权限与 X06 联调未完成（第 3 次作业） |
| LinuxRenderer/地图/仪表盘 | 5（1 号整合复验） | 进行中 | 异步 Qt 渲染与 MapPage 在统一 Ubuntu 测试和真实 Wayland 首帧/截图通过；输入仍为 Mock，真实数据、仪表盘及 OpenGL ES 未验收。**2026-11-11**：金字塔瓦片增加分级面名称（方案 A，DEC-20261111-1-area-labels-01），根 CTest 19/19；Navit 渲染对标受阻（NAVIT-20261111-1-area-labels-01）；同日修复拖动/缩放边缘空白（方案 A 外扩帧+渐进+粗底图，DEC-20261111-1-edge-loading-01）；新增地图清晰度高/中/低（NAVIT-20261111-1-map-clarity-01） |
| LinuxVoiceAdapter/用户页面 | 1，2/4 配合 | 未开始 | 本地 TTS/ASR、FFI、中文与麦克风权限 |
| Linux 发布与最终材料 | 全员，1 汇总 | 未开始 | 逐次收集运行证据、G6 报告和 3–5 分钟视频 |

第一次作业 1 号当前结论（2026-10-26）：核心框架与公共组件开发、Linux 程序及设计/构建材料已完成，可交接其他成员；完整个人验收与团队交付尚未收尾。完整人工键盘、高 DPI/200% 字号属于 design.md 的补验项，其他成员构建复现属于团队环境约定；二者不应混同为任务书新增的独立硬性评分项。真实解析/保存由 2 号实现，1 号在其交付后装配总工程并配合完整导入演示。当前源码尚未提交/推送，不能把核心完成等同为已正式交付。验收依据与剩余事项见日志 `20261026-174106-member1-native-ui-07`（现合并于重要记录对话 4）及对应验收文档。

### 2026-10-18：由原平台任务改为 Linux PAL 适配

- 依据用户的新 PDF 更新正式组名、C++/Qt/CMake 环境、五人职责、四项 PAL 归属、接口表、四次作业与交付清单。
- 1 号接手语音，5 号集中负责渲染；4 号牵头 G6 联调报告。原始 AI 对话保持历史原文，旧任务摘录标记失效，新的正文另存以便查证。
- 验证：提取并视觉核对 PDF 全部 4 页，按任务逐项对应责任人，核查当前文档链接及旧平台术语的残留用途。本次仅修改规划，没有生成或测试业务代码。

### 2026-10-26：项目提示词框架

- 新增 prompts/README.md、start.md、members.md、workflows.md，形成“任务输入 → 上下文核对 → 按职责执行 → 选择与实测 → 验收/交接”的可复用提示词。
- README 和本文增加阅读入口；具体环境、分工、接口和进度仍以项目事实为准，避免模板维护第二套状态。
- 检查模板路径/链接、五人归属、七类流程及当前 Linux 约束；未创建工程或运行业务测试。本次仅文档框架完成。

### 2026-10-26：统一 Linux 桌面虚拟机与工具链

- 按用户选择规定 Linux 桌面虚拟机为共同开发/验收环境，统一 Ubuntu Desktop 24.04 LTS amd64、Qt 6.4.2、GCC/G++ 13.3.0、CMake 3.28.3（C++17）。
- 新增 docs/development-environment.md，记录官方包依据、安装与版本核对、基础镜像/包清单复现、Qt Kit、虚拟机设备验证与升级规则；同步协作规则、README 和提示词。
- 当前仅完成版本约定与文档检查，未安装虚拟机、运行安装命令或构建工程；各成员环境验证状态均未验证。

### 2026-10-26：I 盘基础开发环境完成

- 负责人：1；VirtualBox 7.2.22 / Ubuntu Desktop 24.04.4 amd64；Qt 6.4.2、GCC/G++ 13.3.0、CMake 3.28.3 与 C++17 实测通过。
- 独立检查工程 `~/environment-check/` 通过 CMake 构建、Qt Widgets 桌面运行及 Qt Creator Kit 构建；版本、包修订、截图与本机启动方式见 [环境实测记录](docs/environment/member-1-2026-10-26.md)。
- 为解决安装时多核软锁死，当前使用 1 vCPU / 8 GiB RAM / 100 GiB 动态磁盘，关闭 3D 加速；未关闭 Windows 安全功能。硬件加速、音频/定位/PAL、其他成员复现及导航业务仍未验收。
- 按用户要求本次不记录 ai_log.md；没有替模块负责人选择跨组接口或算法方案。

### 2026-10-26：补充 AI 专业角色设定

- 在 AGENTS.md 第 1.1 节规定按已确认组员编号采用专业角色，分别覆盖应用架构/语音/发布、地理空间数据/存储、Geo 算法/性能、定位/导航/联调、Qt 地图渲染/图形性能，并明确每个角色的工程检查重点。
- prompts/members.md 新增五段“你是一名……”角色提示词；通用开场和框架说明同步角色采用方式。公共规划与规则维护使用项目工程协作助手角色，角色不扩大任务授权或代替接口/算法选择。
- 本次为规则与提示词更新，业务模块状态不变；检查五人角色与既有分工、流程约束一致。按用户要求不修改 ai_log.md。

### 2026-10-26：第一次作业 UI 设计基准

- 负责人：1；能力与入口：[design.md](design.md) v1.0，覆盖主辅背景/文字配色、明暗主题、字体层级、间距留白、按钮/卡片/导航栏样式、地图工作台、数据导入及异常状态。
- 设计来源：用户指定的三份 Dribbble 作品；已读取网页文字与色板，主图未完整载入。文档明确字体、尺寸、阴影和布局为项目设计定义，不声称原稿像素级复刻。
- 协作边界：1 号公共 UI 与框架；2 号导入数据与页面；5 号地图与仪表盘。未修改业务契约，未确认 X01–X09 接入方案或 Qt Widget/OpenGL ES 后端，不触发关键算法试验。
- 修改：新增 design.md；AGENTS.md 增加后续 UI 必读规则；README 和本文补充入口；ai_log.md 追加本轮记录。
- 验证：文档引用、颜色对比度和窗口尺寸算术核对、Git 差异及空白检查；业务 Qt 工程尚未创建，未进行 Linux 界面运行/截图或性能测试。
- 下一步：按规范创建第一次作业 Qt Widgets 外壳与公共组件，再按已确认契约接入 2 号导入流程；地图实现由 5 号负责。业务模块状态保持未开始。
- 关联日志：`20261026-161245-member1-ui-design-04`；本次未提交或推送。

### 2026-10-26：第一次作业应用框架实现与自动验证

- 负责人：1；状态：进行中（源码及 Linux 自动验证通过，原生桌面人工验收待完成）。
- 已实现：根 CMake / C++17 / 精确 Qt 6.4.2；主窗口、QStackedWidget 页面导航、宽/窄侧栏、主题双入口与状态保持；统一卡片/标签/按钮、列表、表单、进度和对话框；系统文件选择入口、元信息与真实空态；模块 QWidget 注册/替换和所有权。入口 `src/app/main.cpp`、`src/app/MainWindow.*`、`src/ui/common/`。
- 边界与数据：导入占位页在 `src/app/ImportLanding.*`，只选择文件，不读取业务内容、不解析或保存；开始导入禁用。`registerPage()` 为 2/3/4/5 号预留整体页面装配接口，不擅自确认 DataRepository、X01–X09 或渲染后端。
- 真实验证：用户授权从 p1-dev-baseline 创建独立链接克隆；Ubuntu 24.04 / Qt 6.4.2 / GCC-G++ 13.3.0 / CMake 3.28.3 / Debug / 1 vCPU / 8 GiB；CMake 构建、Qt Test 7 通过 0 失败、offscreen 启动与 4 张截图、安装后启动及无缺失运行依赖通过。见 [框架说明](docs/assignment-1-framework.md)、[原始报告与截图](docs/verification/assignment1/README.md)。
- 环境异常：原虚拟机根目录 emergency_ro、系统命令 I/O 错误，未修复或回滚；克隆与原机共享基础快照链，不能删除共享基盘，不能未经脱敏分发。见 [环境记录](docs/environment/member-1-assignment1-2026-10-26.md)。
- 交付：来宾安装程序 `~/navigation-assignment1-20261026-1626/out/app/bin/p1-navigation`；宿主 `out/p1-navigation-linux-amd64.tar.gz`（不纳入 Git）；完整构建、测试、安装与交接步骤已写入说明。
- 下一步：用户登录已显示的验证克隆后继续原生桌面/文件对话框/键盘/高 DPI 验收；2 号另行确认真实导入契约并提供页面。算法、地图、定位、语音与离线包均不冒充完成。
- 本轮未新增关键算法，不触发方案试验；未提交/推送，保留原有未提交修改；关联日志 `20261026-162617-member1-framework-05`。

### 2026-10-26：用户登录后的原生桌面补测

- 负责人：1；用户原文“已登录”，中途会话锁定后按要求手动解锁，回复“已解锁”。基础窗口/导航/主题/公共组件子项已完成，框架总体仍为进行中。
- 实际会话：Ubuntu Wayland / navigation，根目录可写；原先统一版本与 VM 资源不变。computer-use 检查真实 VirtualBox 窗口，未操作认证界面或修改安全配置。
- 修正：`MainWindow` 初始尺寸按 `QScreen::availableGeometry()` 限制，较小屏幕下 `main.cpp` 请求 compositor 最大化；截图模式仍显式生成参考尺寸。演示对话框显式采用取消左/确认右，与 design.md 一致。
- 新增回归：`initialClientFitsAvailableScreen()`；最终 offscreen / Wayland Qt Test 均 8 通过 0 失败，重建/安装/安装后启动通过。实际鼠标操作核对浅/深主题、组件页/对话框、Qt 文件选择框、CMakeLists.txt 元信息回传及切换主题保留选择；未将选择文件当成解析成功。
- 证据：[原始日志与桌面截图](docs/verification/assignment1/README.md)，新增 native-*.png、wayland-ui-tests.log，更新 4 张 offscreen 图与程序包；最终校验值见验证记录。实际中文字体匹配为 Noto Sans CJK SC Regular；3D 关闭，Mesa/EGL 警告保留，不作硬件性能结论。
- 限制与下一步：完整人工 Tab 顺序与高 DPI/200% 字号待验收，GNOME portal 选择器未验证；2 号真实解析/保存仍未接入；原机 emergency_ro 仍未修复，其他成员待复现。没有新增跨组方案或关键算法选型。
- 更新 project.md、design.md、框架/环境/验证说明与 AI 日志；未提交或推送；日志 `20261026-174106-member1-native-ui-07`。

### 2026-10-26：统一重要 AI 使用记录格式

- AI 日志按“1. AI使用详情”的九字段格式整理，仅保留推动项目构建的重要成果；工具表逐条对应，并如实标注未知版本。原桌面补测并入框架实现记录，原重要记录 ID 保留，纯补录、登录与重复状态询问不再单列。
- AGENTS.md 第 4 节替换逐轮全文记录规则；具体方案确认仍由 decision_log.md 承载，详细验收依据仍在模块文档。当前业务进度不变，本次整理不新增 AI 日志条目。

### 2026-10-28：位置源契约与可回放模拟位置源

- 日期 / 模块 / 负责人 / 状态：2026-10-28 / LocationSource 契约与 Mock 回放 / 4 / 已完成（Ubuntu 24.04.4 + Qt 6.4.2 + GCC 13.3.0 构建零警告，ctest 3/3 通过）。
- 已实现能力与正式入口：
  - `include/contracts/LocationTypes.h`：CoordinateSystem(WGS84/GCJ02/BD09)、SourceKind(mock/real)、GeoCoordinate、PositionSample，头文件内有字段表（单位、是否可选）。
  - `include/contracts/LocationSource.h`：LocationStatus（Idle/Starting/Active/NoFix/PermissionDenied/Unavailable/Stopped）、LocationErrorCode（权限被拒/服务不可用/位置丢失/精度过低）、LocationError、抽象 `LocationSource`（start/stop/status/kind/sessionId + positionUpdated/statusChanged/errorOccurred）；头注释写明线程归属、所有权/生命周期、stop() 后无回调。
  - `src/navigation/FixedTrackLoader.*`：CSV/JSON 固定轨迹读取，校验坐标范围、时间严格递增、坐标系一致、列名单位；失败返回“来源:行号: 原因”，不抛异常、不跳过坏行。
  - `src/navigation/MockLocationSource.*`：固定步长定时回放；setStepIntervalMs、setSpeed(0.1–32)、pause/resume、seekTo(轨迹帧)、stepOnce 单步、精度阈值、simulateError 故障注入；回放确定性，结束时 replayFinished 后自动 Stopped。
  - `src/navigation/CMakeLists.txt`：目标 `p1_navigation`(STATIC)、演示 `p1-location-demo`、测试 `p1-navigation-tests`；ctest 名 `navigation-location`、`navigation-location-demo`。
  - 样例：`tests/fixtures/location/campus_loop.csv/.json`（合成 12 帧 WGS84 数据，非私人轨迹；内容等价）。
- 接口、数据约定与 PAL 边界：
  - 单位：米、米/秒、度（正北 0 顺时针）、Unix 毫秒 int64。
  - `seq`：会话内发出序号，严格递增，seek 回退也不减。`trackFrame`：仅 Mock 有，表示轨迹帧号，即 seekTo 的参数。
  - `sessionId`：每次 start() 递增，承担 requestId，消费方据此丢弃旧会话样本。`dataVersion` 取自轨迹文件。
  - Mock 时间戳取自轨迹文件，不是墙钟。
  - 含 Q_OBJECT 的契约头由 p1_navigation 负责 moc，使用方链接 p1_navigation。
  - GeoClue2 真实源为替换点（`src/pal/linux/location/`，第 3 次作业），本次目录留空。
- 跨组选择及确认依据：X06/X04/X05/X01/X07 均未接入，按用户要求只用本地 Mock；未触发关键算法选型。
- decision_log.md 记录 ID：不涉及。
- Linux 环境、验证步骤/结果/证据：
  - 环境：VirtualBox 7.2.22 上的 Ubuntu 24.04.4 LTS amd64，Qt 6.4.2、GCC/G++ 13.3.0、CMake 3.28.3、Ninja 1.11.1，2 核虚拟 CPU，`verify-environment.sh` 5 项 PASS。
  - 取码：从仓库分支 `codex/member-4/location-source` 以 `git clone -b ... file:///...` 取得 LF 换行工作副本（`CMakeLists.txt` 中 CR 数为 0），避免 Windows CRLF 混入。
  - 验证命令与结果：
    - `cmake -S . -B build -G Ninja` → 退出码 0。
    - `cmake --build build -j2` → 退出码 0，28/28 目标完成，`-Wall -Wextra -Wpedantic` 下 **0 条 warning**。
    - `ctest --test-dir build --output-on-failure` → 退出码 0，`ui-contracts` 0.47 s、`navigation-location` 0.41 s、`navigation-location-demo` 1.09 s，**3/3 通过，0 失败**。
    - `./build/src/navigation/p1-location-demo` → 退出码 0，24 行输出，末行 `[result] 全部通过`；覆盖 setSpeed(4.0)/25 ms 间隔、stop() 后 400 ms 内回调次数 = 0、stop() 后 stepOnce() 被拒绝且不发信号、seq 严格递增（含 seek 回退）、帧序列 0,1,2,[seek]1,2,3,4,5,6、最终状态 Stopped。
  - 证据：Windows 本机（Qt 6.11 + MSVC 2022）另做的语法冒烟（/W4 无警告、Qt Test 23 通过 0 失败）**不作为验收依据**；验收以上述 Linux 虚拟机结果为准。
- 顶层集成：已在本提交中于根 `CMakeLists.txt` 末尾（`include(CTest)` 的 `endif()` 之后）追加 `add_subdirectory(src/navigation)` 并附注释说明位置约束；该行缺失会导致 `src/navigation` 不参与构建、`navigation-location` 与 `navigation-location-demo` 两项 ctest 不注册。
- 未实现/未验证内容、下一步与涉及组员：
  - 下一步：对本次与 3 号集成后的版本重新进行 Linux 构建与测试；导航状态机（idle/规划中/导航中/偏航/到达）消费 LocationSource；为 5 号提供回放驱动的 NavigationSnapshot。
  - 未实现：GeoClue2、定位权限声明、偏航重算、X06 联调。Mock 不能作为真实定位或 PAL 适配验收证据。
- 关联日志 / 已有提交：`20261028-member4-location-source-01`（ai_log 对话 7，原对话 5）；`412d29d`（位置源契约与 Mock 回放）、`727647f`（根 CMakeLists 引入 `src/navigation`）、`9919b7f`（环境验证记录）已推送至 `pengl337/navigation`，并形成 PR #1（已关闭、未合并）。原先向 origin 推送返回 403 的情况保留为历史；本次由 1 号在主仓库准备保留原提交的集成分支。


### 2026-10-28：第三位组员 P0 私有模型与独立测试入口

- 负责人：3；状态：进行中（原版 P0 审核及用户确认的 Linux 验收通过，收尾补测待 Ubuntu 复验）。用户授权仅实施 P0；分支 `feature/member3-algorithms`，起始提交 `199aa5f`，初轮起始工作区干净；本轮保留初轮未提交的 6 个新增文件及两份记录修改。
- 能力与入口：新增 `src/algorithms/` 下的独立 CMake、`model/Types.h`、`model/Validation.h/.cpp`、`tests/ModelTests.cpp`、README 共 6 文件。目标 `p1_algorithms_model`、`p1_algorithms_model_tests`，CTest `member3-models`；标准库 C++17，不依赖 Qt。
- 内部约定：节点/边 ID 与数组索引分别为强类型；显式数据集/版本/来源与米制坐标框架；有向几何按 from→to。基础校验返回类别、对象和位置，保留合法重复点和相同时间，拒绝逆序、非法数值、零长度边及全退化几何；端点误差容限 `1e-6 m`。发布时深拷贝输入、校验私有副本后转移到只读快照，隔离外部可写别名。
- 初轮 Windows 检查：CMake 4.3.0 / Clang 8.0.1 + MinGW 7.3.0 标准库头文件，两个源文件 C++17 警告及语法检查通过，Debug `-O0 -g`、Release `-O2 -DNDEBUG` 目标文件编译四次均通过。独立 CMake 配置尝试受旧 Make 中文路径编码和 MinGW 工具 DLL 重定位错误阻塞；Windows 链接、测试可执行程序和 CTest 未运行。
- 后续验收：独立审核未发现已确认的阻塞性缺陷；用户确认 Linux 独立构建、模型 CTest、原 Qt 工程回归通过。依据为用户本轮确认及《第三位组员P0独立代码审核报告-2026-10-28.md》第 2.3、7 节；未附原始日志，不补造实际命令、通过数量、构建路径、环境版本、Debug/Release 覆盖或源码指纹对应关系，本轮未重新运行 Linux 验收。
- 最小收尾：仅补 T1 二维容差/终点边界、T2 元数据逐字段空白/opaque 版本原文、T3 剩余坐标轴非有限值、T4 双端点悬空测试。模型及 CMake SHA256 与审核快照一致；修改后的测试在 Windows Clang 下 Debug/Release 目标文件编译均通过，无诊断，未运行链接或用例。详细规则、覆盖和建议复验命令见 [模块说明](src/algorithms/README.md)。
- 下一步：用户在 Ubuntu 共享源码上重跑本轮补测，建议 Debug/Release 独立构建和 CTest，并使用原 Qt 构建目录复验。既往通过记录不表示新增用例已通过；P1 图/几何/样例仍需另行授权。精确长度、邻接、投影、A*、HMM、Qt 服务、页面与性能监控均未实施。
- 接口/方案：模块私有表示不作为公共 contracts；X02–X05 及与 1/2/4/5 号的正式接入约定仍待确认，A＋B 等试验范围未选择。本次不触发关键算法试验，未改 `decision_log.md` 或其他成员模块。
- 关联日志：初轮对话 5，`20261028-member3-p0-models-05`；收尾对话 6，`20261028-member3-p0-closeout-06`。实施记录中的未提交/推送/合并为当时状态；最终提交 `fdd486e` 已于 2026-10-28 18:36（Asia/Shanghai）通过 PR #2 合入 main。

### 2026-10-28：导航状态事件契约、模拟样例与契约测试

- 日期 / 模块 / 负责人 / 状态：2026-10-28 / 导航状态与事件契约（NavigationController / NavigationSnapshot）、五类模拟样例、契约测试 / 4 / **契约已完成**（Ubuntu 24.04.4 + Qt 6.4.2 + GCC 13.3.0 构建零警告，ctest 5/5 通过）；**状态机实现、偏航判定算法、重算去重实现未完成**（属第 3 次作业）。
- 已实现能力与正式入口：
  - `include/contracts/NavigationTypes.h`：NavigationStatus 五态（Idle/Planning/Navigating/OffRoute/Arrived）、NavigationEventKind 16 种 + NavigationEvent、NavigationErrorCode 四类、GuidanceKind 16 种 + GuidanceInstruction、RoutePlanRef、RoutePlanRequest/Result、RerouteRequest、MapMatchRequest/Result、NavigationError、**NavigationSnapshot（13 字段，5 号渲染唯一消费入口）**，各结构带字段表（单位/可选性）、`toString()` 与元类型注册 `registerNavigationMetaTypes()`。
  - `include/contracts/NavigationController.h`：抽象接口（snapshot/status/paused/navigationId/currentRequestId + 10 个命令 start/pause/resume/stop/locationUpdated/requestReroute/routeResultReady/routeFailed/arrived/offRoute + 8 个信号）；3 号 `RoutePlanner` / `MapMatcher` 的调用形状；文件头含线程归属、所有权/生命周期、stop() 后无回调、陈旧 requestId 丢弃四条硬性规则，以及完整状态转换表与已确认的偏航/重算策略。
  - `tests/fixtures/location/`：`normal_straight.csv`(10 帧)、`off_route_recover.csv`(14 帧)、`arrive_destination.csv`(20 帧)、`degraded_signal.csv`(10 帧)、`jump_backward.csv`(10 帧)、`navigation_case.json`(本地 stub 参考路线)、`expected/*.md`(五份逐帧期望值与手算方法)、`README.md`(构造常量、格式约束、三步手算复核法、容差说明)。
  - `src/navigation/tests/NavigationContractTests.cpp`（19 例，ctest 名 `navigation-contracts`）与 `src/navigation/tests/LocationFixtureTests.cpp`（9 例，ctest 名 `navigation-location-fixtures`）。
  - 文档：`docs/navigation-contracts.md`（字段表、转换表、错误码、Mock 替换点、使用示例、验证状态）。
- 接口、数据约定、外部版本和 PAL 边界：
  - 沿用统一约定：距离米、时长秒、速度米/秒、角度度（正北 0 顺时针）、时间戳 Unix 毫秒 int64；经纬度带坐标系标记；全部带 requestId/navigationId/dataVersion/source。
  - **Paused 是 Navigating/OffRoute 的子标志，不是独立状态**（`snapshot.paused` 与 `statusChanged(status, paused)` 第二参数），理由见 `docs/navigation-contracts.md` 第 2 节。
  - `navigationId` 每次 start() 递增；`requestId` 每次 start()/requestReroute() 递增；只有 `requestId == currentRequestId()` 的结果被采纳，其余丢弃并发 Rejected 事件 —— 落实"旧异步结果不得覆盖当前导航"。
  - 偏航/重算策略（已确认）：横向偏离 > 15 米且持续 ≥ 3 秒才判定偏航；重算最小间隔 10 秒；在途 requestId 未回流前不发起新请求；间隔按**定位样本时间戳**计算以保证回放确定性。
  - 路网几何格式属 2/3 号，本契约只透传 `RoutePlanRef::geometryRef` 不透明字符串。**偏离量由 3 号 MapMatcher 提供，本契约只定阈值与持续时间。**
  - `LocationSource`（Mock 与真实 GeoClue2 同接口）的 `positionUpdated` 直接接 `NavigationController::locationUpdated()`；切换真实源时导航侧零改动。GeoClue2 替换点仍为 `src/pal/linux/location/`（第 3 次）。
- 跨组选择及确认依据：本次不新增跨组接入。X06（G6）未确认，**本次未接入、未联调**；X04/X05（3 号 A*/HMM）只定义调用形状并配本地 stub，未替 3 号选算法；X01/X07 属 2 号，未涉及。关键算法策略由组员 4 于 2026-10-28 从三个候选中选定候选 2（原文"选择2。进行下一步"）。
- decision_log.md 记录 ID：`DEC-20261028-4-offroute-reroute-01`（偏航判定与重算去重策略；状态"已确认"，因当前无真实路网与噪声样本、未做 2–3 种试验对比，按 AGENTS.md 7.3 未标"已完成"）。
- Linux 环境、验证步骤/结果/证据：
  - 环境：VirtualBox 7.2.22 上的 Ubuntu 24.04.4 LTS amd64，Qt 6.4.2、GCC/G++ 13.3.0、CMake 3.28.3、Ninja 1.11.1，2 核虚拟 CPU。
  - 独立编译单元检查（两份契约头，`-std=c++17 -Wall -Wextra -Wpedantic -fPIC`）：退出码 0，**warning 0**。
  - `cmake -S . -B build -G Ninja` → 退出码 0；`cmake --build build -j2` → 退出码 0，**warning 计数 0**；`ctest --test-dir build --output-on-failure` → 退出码 0，**5/5 通过**：ui-contracts、navigation-location、navigation-location-demo、**navigation-contracts（19/19）**、**navigation-location-fixtures（9/9）**。
  - AUTOMOC 证据：`build/src/navigation/p1_navigation_autogen/.../moc_NavigationController.cpp` 已生成并编入 `libp1_navigation.a`（含 Q_OBJECT 的契约头已在 `src/navigation/CMakeLists.txt` 显式列出）。
  - 样例实测（一次性检查程序，写在 `/tmp`，不入库）：normal_straight 末帧进度 90.1692 m / 剩余 100.1880 m；off_route_recover 峰值偏离 20.0376 m 且连续 3 帧超 15 m；arrive_destination 末帧进度 190.3572 m / 剩余 0.0000 m；degraded_signal 最大帧间空档 30 s、3 帧零位移、3 帧 35 m 精度；jump_backward 最小帧间位移 −20.0376 m；`navigation_case.json` 被误当轨迹加载时明确报错（不静默解析）。
  - 回放实测：degraded_signal 触发 3 次 `AccuracyTooLow`，末态 Stopped；jump_backward seq 单调、末态 Stopped。
  - 证据位置：`docs/navigation-contracts.md` 第 12 节；测试源码 `src/navigation/tests/`；样例与期望值 `tests/fixtures/location/`。
- 未实现/未验证内容、下一步与涉及组员：
  - 未实现：状态机实现、偏航判定与重算去重代码、真实 GeoClue2 源、定位权限、G6 联调（X06）。到达半径、长时间无定位阈值、精度过低阈值仍未确定（属第 3 次）。
  - 未验证：跨线程排队连接的运行期实测（当前无后台线程场景）；`registerNavigationMetaTypes()` 运行期未调用；真实 3 号规划/匹配未接入；阈值 15 m/3 s/10 s 未经真实噪声数据标定。
  - 涉及组员与下一步：第 2 次作业由 **3 号**用真实 RoutePlanner/MapMatcher 替换本地 stub（控制器侧无需改动），**5 号**消费 `NavigationSnapshot` 做地图与引导展示，均可并行；第 3 次作业由 **4 号**照转换表实现状态机并接真实定位。
  - 关联提交：导航契约部分经 **PR #4** 于 2026-10-28 19:29（UTC+8）**合入 `main`**（提交"4号：导航状态事件契约增量（基于最新main，导航契约记录占对话8）"，合并节点 `c0886c8`；具体哈希见 `main` 提交历史，文档内不固化自身提交哈希）；位置源部分见 `main` 上的 `412d29d` / `727647f` / `9919b7f`。
- 关联日志 / 已有提交：`20261028-member4-navigation-contracts-01`（ai_log **对话 8**；原拟编号 6，因 3 号占用对话 5 / 对话 6；1 号集成时曾以其合并记录占对话 8，该记录已在 main 上删除、编号 8 空出，故本记录占用对话 8）。位置源三提交（`412d29d` / `727647f` / `9919b7f`）已由 1 号于2026-10-28 通过 PR #3 合入 `main`，本分支不再重复提交；导航契约部分本次以增量形式提交到上游 `codex/member-4/location-source` 并开 PR 请 1 号评审合并（Windows 侧 `github.com` https 不通，走 SSH）。
- 与 3 号模块的关系（2026-10-28 核对 `feature/member3-algorithms` 的 `fdd486e` 后记录）：3 号 P0 为模块私有米制模型（`p1::algorithms::model`，`LocalCartesian/ProjectedCartesian`，单位米，明确"不作为公共 contracts"、未接入根 CMake），与 4 号契约无代码冲突；双方坐标单位不同（4 号度 / 3 号米），分工见 `docs/navigation-contracts.md` 第 6.1 节：**4 号不转换，由 3 号适配层负责**。

### 2026-10-31：确立 Navit 持续性能对标规则

- 范围与负责人：团队公共规则维护；发起组员编号待确认。影响 1–5 号后续全部功能模块，1 号维护共同基线，各模块负责人提供比较证据。
- 产出：AGENTS.md 第 8 节规定对应能力核查、固定版本/配置、同一 Linux 虚拟机与同源数据、正确性/质量先验、性能差异与原始证据、受阻/不适用记录和验收衔接；新增 `docs/navit-benchmark.md` 的指标建议、报告模板及索引，同步提示词流程与方案抉择模板。
- 当前事实与限制：本次是文档工作，未安装/构建/运行 Navit，未执行性能比较，没有新增业务实现或改变跨组接口、算法方案。实测工作仍未开始，历史完成记录不改写为已有 Navit 证据。
- 验证：核查相关规则、模板及项目引用的一致性，执行文档链接与 Git 差异/空白检查；不以文档检查代替 Linux 性能实测。
- 下一步：1 号建立并记录共同 Navit 运行基线，各负责人为后续功能登记等价任务、指标、测试数据与报告；缺少条件按规则保留补测事项。
- 关联日志：ai_log 对话 9（`20261031-navit-benchmark-rules-01`）；无本次算法选型，Navit 对标确认原文见 `docs/navit-benchmark.md` 第 1 节。

### 2026-11-01：1 号第二次作业首轮应用集成准备

- 负责人：1；状态：进行中。代码基线 `ca12fe6` / `main`，任务开始已有的规则、日志与项目文档修改全部保留；本轮未提交或推送。
- 已实现：根 CMake 装配 3 号现有基础模型目标/测试；移除主窗口对 MapDemoWidget 的默认注册，使尚未接入的地图入口保持禁用；强化 `MainWindow::registerPage()` 的 GUI 线程、单页单路由、祖先/子控件拒绝、替换销毁与同页元信息更新规则；补齐三个生命周期/集成测试，沿用原视觉令牌并修正紧凑侧栏标签和状态栏换行。入口为 CMakeLists.txt、src/app/MainWindow.h/.cpp、src/app/main.cpp、tests/UiTests.cpp。
- 接口与影响：继续使用已有 QWidget 页面注册 API；3/5 号提供独立目标与工厂，由 1 号在应用装配，窗口不访问其内部数据；2 号导入页面仍待真实契约与装配，未直接启用仓库中未进入根目标的 ImportPage/OsmxParser。语音只整理第三次作业的后端、FFI、生命周期和权限交接清单，未选择 X09/G10 或 X06/G6 协议，未新增公共语音 ABI。
- 跨组/算法选择：未新增外部接入，也未新增或替换关键算法，不触发本轮候选试验或新的 decision_log 记录；X04/X05 算法来源、5 号渲染后端与 X06/X09 决定由对应负责人确认。
- 实测：用户确认虚拟机已启动；SSH 核对 P1-Ubuntu-24.04 当前根目录为 rw,relatime，团队 Ubuntu 24.04.4 / Qt 6.4.2 / GCC-G++ 13.3.0 / CMake 3.28.3 / Ninja 1.11.1，Debug / 1 vCPU / 8192 MiB / -j1。基线 UI 复现 6 通过、2 失败；最终根 CTest 6/6、offscreen 与真实 Wayland UI 各 11 通过/0 失败（含初始化/收尾），源码编译 warning 0；BUILD_TESTING=OFF 完整构建通过、注册 0 项测试；安装后 offscreen/Wayland 启动与 ldd 无缺失通过。最终源码 57 文件 SHA256 与来宾逐项核对一致。
- 证据与产物：[集成/交接说明](docs/assignment-2-integration.md)、[原始日志、脚本与 Wayland 窗口抓图](docs/verification/assignment2/README.md)；宿主忽略目录 `out/p1-navigation-assignment2-linux-amd64.tar.gz`，来宾安装入口 `/home/navigation/p1-assignment2-20261101-member1-01/install/bin/p1-navigation`。包只含已验证外壳，不含尚未接入的业务/离线包，依赖目标 Ubuntu 的 Qt 运行库。
- 限制：Mesa/EGL 运行提示仍在，3D 关闭，不声明硬件加速或 OpenGL ES 验收；此次可读写不代表历史文件系统故障根因已修复。真实 A*/HMM、LinuxRenderer、业务非阻塞/取消/过期结果、性能监控及完整团队演示尚未接入。1 号首轮接入准备通过正确性检查，第二次作业整体验收保持进行中。
- Navit：`NAVIT-20261101-1-integration-shell-01`，受阻（未测）；当前 VM 未安装 Navit，版本/配置及同源业务场景未建立，无耗时/内存/吞吐量差值。报告保存缺口及补测步骤，未把 CTest 时长当成性能数据。
- 下一步：3 号提供真实服务、算法页/监控与模块 CMake；5 号提供真实 LinuxRenderer、地图页/工厂、线程/上下文与绘制证据；2 号提供数据。1 号据公共契约完成装配并建立 Navit 共同基线，各负责人补测等价任务。关联日志：对话 10（`20261101-member1-assignment2-integration-01`）。

### 2026-11-01：4 号第二次作业规划对接层

- 负责人：4；状态：进行中（对接层子项完成，状态机属第 3 次）。代码基线 `main`/`cb056d0`，分支 `codex/member-4/assignment2-integration`。
- 已实现：`src/navigation/AsyncPlannerBridge.{h,cpp}`——规划请求线程化对接桥：控制器线程（GUI）发出 `routePlanRequested`/`routePlanCancelled`，经队列连接投递至内部 QThread 上的 `RoutePlanner`，结果 `routeReady`/`routeFailed` 经 `Qt::QueuedConnection` 回流控制器线程（落实契约文件头 ①线程规则）；析构先退线程再回收，析构后零回调。`StubRoutePlanner.{h,cpp}`——确定性规划桩：延迟/失败/取消语义（`setHonorCancel(false)` 模拟取消不及时）可注入，haversine 距离确定性可复算；`RecordingController.h`——对接验证桩（非第 3 次状态机）；`p1-planner-demo` 演示程序；`tests/PlannerBridgeTests.cpp` 7 例集成测试。
- 接口与影响：未新增契约头——对接缝即第一次契约的 `contracts::RoutePlanner` 抽象接口，3 号真实 A*/HMM 实现该接口后桥/控制器/测试零改动替换（DEC-20261101-4-planner-bridge-interface-01，方案 B 契约先行）。异步模型 QThread+Worker（DEC-20261101-4-async-model-01，本次忽略）。CMake 变更仅 `src/navigation/`：库源 + `p1-planner-demo` + `p1-navigation-integration-tests`（ctest `navigation-integration`、`navigation-planner-demo`），顶层 CMake 未动。
- 实测：统一虚拟机 p1-linux-member4（Ubuntu 24.04.4、Qt 6.4.2、G++ 13.3.0、CMake 3.28.3、Ninja、Debug、-j2）：configure 0 / build 0 / **警告 0**；全量 ctest **9/9**（新增 navigation-integration 7 例全过、navigation-planner-demo 通过）。演示输出：300ms 模拟计算期间主线程心跳 4 次（UI 循环不阻塞）、`stub-route-1` 100.08 m、stop() 后事件数不变。
- 证据与产物：`docs/benchmarks/navit/NAVIT-20261101-4-planner-bridge-01.md`（对标受阻未测及补测步骤）；ai_log 对话 13（`20261101-member4-planner-bridge-01`）。
- 限制：RecordingController 为验证桩，完整状态机属第 3 次；偏航触发/重算去重的端到端场景未建；Navit 对标受阻（VM 未安装 Navit、stub 非真实算法），真实算法接入后补测规划 P50/P95 与取消响应。
- 下一步：3 号确认 X04/X05 并交付 `RoutePlanner` 实现后联调替换；第 3 次作业按契约第 ⑤ 节实现状态机（ReferenceController 脚手架为底稿）。

### 后续完成记录模板

```text
日期 / 模块 / 负责人 / 状态：
已实现能力与正式入口：
接口、数据约定、外部版本和 PAL 边界：
跨组选择及确认依据：
decision_log.md 记录 ID（如涉及）：
Linux 环境、验证步骤/结果/证据：
Navit 对标报告 ID、双方基线、主要差异、限制/未测或不适用依据：
未实现/未验证内容、下一步与涉及组员：
关联日志 / 已有提交：
```

### 2026-11-01：标准 OSM 导入替换（进行中）

负责人：1 号授权装配，影响 2 号导入与 3/5 号数据消费。X01 选 B，本次忽略方案对比并明确采用 libosmium 2.20.0＋SQLite；见 DEC-20261101-1-import-backend-01。真实导入页面替换占位注册，支持 .osm/.pbf/.osm.pbf/.osm.gz/.osm.bz2；后台解析、取消、事务临时缓存、重启目录读取及公共只读 API 已编码。入口 src/data/import/、src/app/ImportPage.*、include/contracts/ImportedDataset.h；约定及复现见 docs/osm-import.md。

恢复前阶段记录：统一 Ubuntu 完整构建成功，运行测试待恢复 VM 后执行（当前黑屏、SSH 无响应）；湖北 PBF、UI 实际显示与 Navit 对标均未验证，不能标已完成。原始 OSM 缓存不生成可路由图，不宣称完整 StorageAdapter、G7 离线包或算法/渲染已集成。旧实现源码暂留，待验证后清理。下一步由 1/2 号恢复环境后执行 CTest、真实数据导入、安装与对标；报告 NAVIT-20261101-1-osm-import-01，AI 日志 11。未提交/推送。

本轮恢复后结果：标准 OSM 导入子项已完成。Debug/Release CTest 各7/7，导入专项17/17、真实Wayland UI12/12；湖北节点9077243、道路593776、关系5262，缓存完整性ok，3类缺失关系引用警告保留。旧 OsmxParser/ImportLanding 四文件已清理，正式路径不再引用；61文件源码指纹一致。新版安装于 /home/navigation/p1-import-libosmium-20261101/install，现有P1快捷方式已更新并启动，重启恢复列表。Navit报告已完成本轮各自预处理测量，P1中位41.28秒/P95 215.78秒，Navit47.35/49.63秒；因snap-store负载干扰和输出语义不同不计算优劣。历史受阻已解除，完整StorageAdapter/G7离线包、路由图/地图消费仍未实现，模块整体保持进行中。证据见同名报告目录，日志11合并本任务结果，未提交/推送。

### 2026-11-01：LinuxRenderer 无头场景与 A/B 模式

日期 / 模块 / 负责人 / 状态：2026-11-01 / LinuxRenderer、MapPage 与异步渲染 / 5 / 进行中。

已实现能力与正式入口：新增 `src/pal/linux/renderer/` 模块内 `MapScene`、Mock 场景、`LinuxRenderer`、最新 requestId 去重、后台 QImage 绘制和 `NavigationSnapshot` 覆盖层；新增 `src/ui/map/MapPage.*` 页面工厂并注册到主窗口 `map` 入口。正式地图页固定使用缓存底图，不显示 A/B 选择。旧 `MapDemoWidget.*` 保留，未删除。正式入口为 CMake 目标 `p1_renderer`、`p1-renderer-tests`；内部基准默认不构建。

接口、数据约定和 PAL 边界：本轮采用 I1，由 `MockMapScene` 提供显式 `mock` 的 1024×640 场景、8 张瓦片、1 条路线和 3 个标记；`NavigationSnapshot` 只消费已有字段。MapScene 暂为模块内契约，真实 StorageAdapter、A*/HMM 结果和 G8/X08 接口未接入，不把 Mock 作为真实联调证据。

跨组选择及确认依据：用户 2026-11-01 选择接口 I1；随后明确不需要 A/B 比较，只保留符合第二次作业需求的正常运行流程。地图页固定使用缓存底图；完整重绘代码保留作开发期正确性对照，但不显示入口。Navit 只作架构参考，未复制 GPL v2 源码。

decision_log.md 记录 ID：`DEC-20261101-5-renderer-01`，用户已取消界面比较；正式路径固定为缓存底图。

Linux 环境、验证步骤/结果/证据：Windows + Qt 6.12.0 + MinGW GCC 13.1.0 独立冒烟工程中，渲染测试通过，MapPage 单一入口异步首帧通过。Windows 数据不作为统一 Ubuntu/Qt 6.4.2 验收；统一 Linux 构建、CTest 和 Wayland 截图待补测。

Navit 对标报告 ID、双方基线、主要差异、限制/未测或不适用依据：`NAVIT-20261101-5-renderer-01`。用户取消本轮 A/B 比较，普通构建不生成基准工具；Navit 仅用于接口和渲染流程参考。

未实现/未验证内容、下一步与涉及组员：真实地图瓦片、路线/轨迹数据、导航快照运行时、G8/X08、平移缩放、OpenGL ES 和 Linux 运行验收待完成；需 1/2/3/4 号提供装配与数据接口。下一步在统一 Ubuntu 构建运行 `p1-renderer-tests` 并检查 Wayland 地图页面，不要求运行性能比较。

关联日志 / 已有提交：ai_log 对话 14，`20261101-member5-linux-renderer-01`；本轮仅在本地分支修改，未提交或推送。

### 2026-11-01：三号算法模块成果与交接

3 号负责 C++ A*/HMM 集成、坐标与数据转换、后台计算及算法调用/性能页。已完成 Boost.Graph A*、Qt RoutePlanner、外部固定 FMM 的有限窗口 Qt MapMatcher，以及取消、过期结果过滤和结果所有权管理；源码在 `src/algorithms/`，算法提交 `1ab1cbe` 已推送个人分支。Ubuntu 24.04／Qt 6.4.2／G++ 13.3.0／CMake 3.28.3／Boost 1.83.0 下，A* Debug/Release 各 3/3，完整独立模块五项 CTest 5/5；三条真实 FMM 新输出与官方合成轨迹预期一致，15 个原始证据文件及 SHA256 保存在仓库外 `../navigation-仓库审查资料/member3-acceptance-evidence-20261101-145035/`。合并前根回归为 4/5，UI 的 map 入口与页面栈数量两断言失败；本次通过合并提交 `ad4073d` 保留组长 `9cc04b1` 的导入/页面更新与算法成果；用户最新确认 Ubuntu 独立算法 CTest 5/5、根工程 CTest 7/7、失败 0，新增依赖 libbz2-dev、libosmium2-dev 已补齐。这是合并后新结果，来源为用户反馈；未补写新耗时、构建模式或用例明细。公共 RoutePlanner/MapMatcher 签名保持，生产 WGS84↔米制投影、可路由图/UBODT 来源和 ID 映射由 2/3 号交接；4 号接请求、会话重置和空匹配处理，既定路线偏离量仍待路线几何；5 号确认几何转换与只读句柄，1 号负责页面和根工程装配。算法页、性能监控、真实导航联调及三号 Navit 对标尚未完成，模块整体为进行中。使用说明见 [A*](src/algorithms/ASTAR_INTEGRATION.md)、[FMM](src/algorithms/FMM_INTEGRATION.md)，选型见 `DEC-20261101-3-open-source-01`；AI 记录：A* 合并后对应对话 15，FMM/HMM 对应对话 16（原记录 ID 保留，避免与 4/5 号编号冲突）。

### 2026-11-03：五人第二次作业源码整合与共同验证

负责人：1；保留 2 号标准 OSM 兼容的 OSMX 交付、3 号 c7ee852 算法分支、4 号桥和真实 A* 对接、5 号渲染源码。主线基线 e3fb6a6，在 codex/member-1/assignment2-team-integration 完成已有合并流程。日志/进度冲突保留双方内容，3号日志顺延15/16并保留原记录ID。入口及数据边界见 docs/assignment-2-team-integration.md；源文件不再依赖交付压缩包解压，历史压缩包保留未删。

装配/共同验证子项已完成：Release完整构建、最终CTest17/17、真实Wayland UI13/13、OSMX页面4/4、BUILD_TESTING=OFF构建/启动、安装及库依赖检查通过；122源码指纹0差异，原始证据见 docs/verification/assignment2-team/。已有MapScene未使用函数warning1条保留；OSMX真实样例缺失跳过1项，真实FMM官方样例未注册。新版路径 /home/navigation/p1-team-final-20261103/install/bin/p1-navigation，既有桌面/菜单快捷方式已更新。

功能、验收和下一步：共同构建与模块回归已通过，完整第二次作业仍进行中。1号算法调用页面与性能面板、2/3号生产路由图/ID/坐标转换、3号真实FMM配置、5号真实MapScene与4号导航快照连通均未完成；不把Mock地图或进程fixture当作完整导航。Navit报告 NAVIT-20261103-1-team-integration-01 如实记录未测与恢复条件，不生成性能倍数。关键算法/外部方案未变更；用户已授权最终提交与上传，日志17记录本轮成果。

### 2026-11-11：标准 OSM 要素导入（POI / 道路 / 面）

- 负责人：1 号授权跨模块修改；模块主责 2 号，消费方 3 号（`roads()`）、5 号（`MapGeometry.h`）。状态：**进行中**（功能已实现并通过自动化测试，目标环境未验收）。
- 能力：libosmium 2.20.0 两遍读取 `.osm`/`.pbf`/`.osm.gz`/`.osm.bz2`，提取 POI、道路、面；面由 libosmium 组装；POI 只入库不上图。
- 代码/入口：`src/data/import/OsmImportParser.cpp`、`FeatureRules.h`、`ImportedDataset.cpp`、`ImportedRoads.cpp`、`MapGeometry.cpp`、`MapAreas.cpp`；契约 `include/contracts/ImportedDataset.h`；页面 `src/app/ImportPage.cpp`；命令行 `p1-import-cli`。
- 接口与数据约定：WGS84 度、有符号 OSM ID；缓存 schema 2 私有；契约删除原始对象读取、新增 `OsmPoi` / `pois()` / `findPois()`；`ImportStats` 新增 `pois`/`roads`/`areas`/`areasSkipped`。跨组接口 X01 沿用方案 B（本组读取标准 OSM），未变更。
- 测试：云端容器 Release、offscreen，根 CTest 19/19；导入专项 19 用例、金字塔 7 用例通过；合成 31 MB 样例导入约 1.5 s（非基准）。
- 未实现/未验证：统一 Ubuntu 虚拟机复验；湖北真实 PBF 的耗时、内存与面质量；POI 搜索界面；按瓦片的 POI 空间索引；Navit 对标。
- 下一步：2 号/1 号在统一虚拟机复验并重新导入真实数据；需要 POI 搜索页面时由 1 号基于 `findPois()` 实现；补 Navit 同源对标。
- 记录：`DEC-20261111-1-osm-feature-import-01`、`NAVIT-20261111-1-osm-feature-import-01`、AI 日志 18。

### 2026-11-11：地图面名称分级显示

- 负责人：1（本对话确认编号）；影响 5 号的瓦片绘制，2 号的数据契约不变。状态：**进行中**。功能、回归和本组性能已完成；Navit 渲染对标受阻。
- 能力：水域/绿地/土地利用/建筑中有名称的面，在地图上按层级显示名称；名称落在面内，相互避让，跨瓦片无缝，明暗主题都适配。
- 代码/入口：`src/ui/map/AreaLabels.{h,cpp}`（标注点、层级、索引表、严格避让、绘制）；`src/ui/map/TilePyramid.cpp`（建索引时写入 `area_labels`/`label_bounds`，`generate()` 中烘焙名称）；`src/ui/common/Theme.{h,cpp}` 新增 `waterLabel`、`labelHalo`；测试在 `tests/TilePyramidTests.cpp`。
- 数据约定：读取 `MapArea::name`（`name:zh` 优先）与 `minZoom`。标注点为 Web Mercator 米，出现层级 = max(种类层级, 标注点到边界 ≥ 10 px 的首级)，建筑 ≥ 16，>18 级的不入库。派生缓存升级为 `xyz-v3-labels1` / `raster-style4-labels`，旧缓存自动重建，源缓存与路由图不受影响。
- 方案：`DEC-20261111-1-area-labels-01`。试验组合 A 瓦片内烘焙、B 帧级叠加；最终选 A，B 及试验开关、基准程序已删除，对比数据留存。
- 测试：统一基线（Ubuntu 24.04.5 / Qt 6.4.2 / GCC 13.3.0 / CMake 3.28.3，Release）根 CTest 19/19，金字塔 11 用例（新增面内锚点、层级、严格避让与查询窗口无关、分级与跨瓦片接缝 4 项）；offscreen 启动检查通过。湖北 PBF（SHA256 `da2b0200…0297ab`）实测：冷帧 142–1435 ms（瓦片生成主导），热帧中位 2.3–19.4 ms，峰值 RSS 208–239 MiB；名称标注点提取使索引构建增加约 1.6 s（17,751 个名称）。
- 道路名修正（同日，组员 1 要求“使距离近的相同道路名称合为一个”）：道路名改为建索引时在每段道路一半长度处取标注点（表 `map_labels`，含 `road` 列），与面名称一起严格避让；同名标注点相距 < 256 px 只保留优先级最高者（道路长者优先），删除原先按瓦片逐段绘制、最多 4 个的道路名。缓存升级为 `xyz-v4-labels2` / `raster-style5-labels`。金字塔测试 13/13（新增同名合并单元测试与“三段同名道路只显示一次”集成测试），根 CTest 19/19。湖北数据：索引 + 首帧 6944 → 7416 ms；武汉中心 z15 冷帧中位 370 → 501 ms（候选更多、查询范围扩到 339 px），热帧不变（约 3 ms），显示名称 34 → 52（含道路名）。
- 未实现/限制：每个面只有一个标注点，超大面（如丹江口水库）在高层级视野内可能看不到名称；道路名为水平文字，不沿道路方向排布；几何瓦片接缝处的细线早已存在，本次未处理；真实 Wayland 桌面人工目测未做；Navit 渲染对标未测。
- 下一步：1 号补 Navit 同视野渲染对标；需要时将道路名改为沿线排布；5 号复核样式。
- 记录：`DEC-20261111-1-area-labels-01`、`NAVIT-20261111-1-area-labels-01`、AI 日志 19。

### 2026-11-11：拖动/缩放时的边缘空白修复

- 负责人：1；影响 5 号地图显示。状态：**进行中**。功能、回归和本组性能已完成；真实桌面人工目测与 Navit 对标未完成。
- 原因：请求只覆盖可见范围；未缓存的瓦片要全部生成完才发布；缩小时除 8–10 级外没有粗略底图。
- 方案：`DEC-20261111-1-edge-loading-01`，试验 A 外扩帧+渐进+粗底图、B 后台预取环+渐进；组员 1 选 A，B 的预取分支、策略枚举、环境开关和可变请求延迟已删除。
- 代码：`src/ui/map/TilePyramid.{h,cpp}` 的 `frame()`：四周外扩 256 px；缺失瓦片先生成 z-2 粗略瓦片，再按离中心距离生成；每 100 ms 渐进发布。`PyramidStats` 新增 `blank`（可见区内无图瓦片数）与 `complete`（请求的最终帧）；`frameReady` 的 view 为图像实际覆盖范围（大于请求）。`MapView.cpp` 新增画布属性 `tilesComplete`。请求延迟保持 180 ms。
- 测试：新增 `panEdgesAreCovered`（平移 40% 与缩小 1.25 倍后，首帧可见区无空白瓦片、四角为面图层颜色）；已有金字塔/业务测试改为等待完成帧，并按帧的实际覆盖范围裁取。金字塔 14/14，根 CTest 19/19。
- 湖北实测（武汉中心 1280×800，冷缓存，3 轮 × z13/z15，见 `NAVIT-20261111-1-edge-loading-01`）：平移时首帧空白中位 0%（最大 10%），空白消失中位 21–25 ms；缩小 2 倍时首帧空白 0% / 11%，空白消失 52 / 376 ms；完全绘完的代价：平移 131–251 ms、缩小 1.5–2.3 s，峰值 RSS 253.8 MiB。
- 限制：比原先多生成约一圈瓦片，CPU 与磁盘缓存占用增加；z15 大幅缩小时首帧仍有约 11% 空白，需等粗略瓦片生成；画布在拖动的一瞬间仍只平移上一张视野大小的帧，外扩内容要等约 30 ms 的重绘。
- 下一步：在真实 Wayland 桌面目测拖动/缩放；补 Navit 同场景对比。
- 记录：`DEC-20261111-1-edge-loading-01`、`NAVIT-20261111-1-edge-loading-01`、AI 日志 20。

### 2026-11-11：地图清晰度 高/中/低

- 负责人：1；影响 5 号地图显示。状态：**进行中**。功能、回归和本组性能已完成；真实桌面目测与 Navit 绘制耗时对比未完成。
- 原因：层级按 `floor` 选取，256 px 瓦片在屏幕上放大 1–2 倍，合成时又没开平滑缩放，所以发糊。实测显示器为 2560×1440、缩放 1。
- 实现：`TilePyramid` 新增 `MapClarity{Low,Medium,High}`、`setClarity()`、`clarityDensity()`。瓦片按密度 1/1.5/2 栅格化（256/384/512 px），绘制坐标仍在 256 逻辑空间，文字和线宽在屏幕上的大小不变；缓存键为 `raster-style5-labels-d<像素>`，三档分开存放，几何索引共用。切换时重建后端并立即重请求当前视野。底图合成开启 `SmoothPixmapTransform`，因此“低”档放大时由最近邻改为平滑插值，与之前略有不同。`MapView.cpp` 的地图菜单新增“地图清晰度”子菜单（`mapClarityHigh/Medium/Low`，互斥单选），默认低；设置只在本次运行内保持，没有写入用户偏好，偏好持久化应走 2 号的 StorageAdapter。
- 方案流程：只增加既有金字塔的分辨率参数，算法选择不变；三档由用户指定，未按第 7 节开多方案试验。
- 测试：`clarityRendersDenserTiles`（切到高后 PNG 为 512 px、层级不变、名称在屏幕上的宽度与低档相差 ≤ 25%）；业务测试 `clarityMenu`（三项互斥、默认低、选高后完成重绘并生成 512 px 瓦片）。金字塔 15/15，根 CTest 19/19。
- 湖北实测（`NAVIT-20261111-1-map-clarity-01`）：冷绘完 z13 低/中/高 = 2.0 / 2.7 / 3.3 s，z15 = 1.2 / 1.7 / 2.1 s；热帧均为 10–15 ms；单视野 PNG 体积约 1 : 1.7 : 2.5；峰值 RSS 96 / 129 / 145 MiB。截图中高档文字与边线明显更锐利。
- 限制：设置不跨重启保存；高档首次加载更慢、磁盘缓存更大（2 GiB 上限不变，三档共用）；没有定量清晰度指标；没有处理 HiDPI（devicePixelRatio > 1）的屏幕。
- 下一步：在真实桌面目测三档；需要时由 2 号提供偏好存储接口后保存选择；补 Navit 对比。
- 记录：`NAVIT-20261111-1-map-clarity-01`、AI 日志 21。

### 2026-11-11：路线模式（一般模式 / 行车模式）

- 负责人：1（用户授权的跨模块修改）；路径规划主责 3、导航 4，**需提示 3、4 号评审**。状态：**进行中**。功能与自动化测试完成；组员 1 确认不补做第 7 节方案对比（原文“不用，就这样”）；Navit 规划耗时对比受阻。
- 能力：地图菜单“路线模式”。一般模式为原行为；行车模式参照 Navit 0.5.7 car 车辆配置，只用机动车可通行的道路规划。
- 代码/入口：`src/app/RoutingProfile.{h,cpp}`（`RouteMode`、`carAllowed()`）；`NavigationWorkflow::setRouteMode()/routeMode()/drivingGraph()` 与 `plan()` 中的行车分支（子图 + 车行道吸附）；`MapView.cpp` 菜单 `mapRouteModeMenu` / `routeModeGeneral` / `routeModeDriving`。A* 与 `RoutePlanner` 契约未改，FMM 匹配仍用全网。
- 规则：允许 motorway…tertiary（含 link）、unclassified、residential、living_street、service、track（grade4/5、ground/grass 除外）、ferry；排除 pedestrian、footway、path、cycleway、steps、bridleway、construction 及其他类型；`motorcar/motor_vehicle/vehicle` 取 no/private/agricultural/forestry/delivery/destination 时排除。逐项对照见 `NAVIT-20261111-1-route-mode-01`。
- 测试：`drivingModeExcludesNonCarRoads`、`routeModeMenu`；根 CTest 19/19。
- 湖北实测（武汉中心 3 km，40 对随机起终点）：一般模式 35 条成功路线中 19 条经过非机动车道路（中位 180 m）；行车模式全部避开，距离中位 1.001 倍、最大 1.352 倍；A* 中位 3.49 → 2.43 ms，构建子图约 1.1 ms。
- 限制：没有速度剖面，仍按最短距离；`access=no` 的道路在导入层已不可规划，所以 specific yes 的覆盖不起作用；`highway=road` 不参与规划；模式不跨重启保存。
- 规划区域自动扩大（同日修复，组员 1 要求“修复”）：原先标准 OSM 规划只加载起终点外框 + 1 km 的道路，路线需要绕出该范围时报“无法规划：no directed path”。`NavigationWorkflow::plan()` 在 A* 不可达时按 2 → 4 → 8 km 扩大区域重试，直到超过 `buildNetwork` 的规模上限；成功时状态注明扩大到几 km，仍失败时注明“已扩大到 8 km 仍不连通”。A*、吸附与路线模式规则不变，OSMX 全量数据不受影响。测试 `planningExpandsAreaWhenRouteLeavesIt`；根 CTest 19/19。湖北复测：武汉 40 对随机起终点中原先会报“无法规划”的 6 对全部成功，扩大后的规划中位 116 ms（未扩大 50 ms）；见 `NAVIT-20261111-1-route-mode-01`“规划区域自动扩大”一节。剩余失败为选点 500 m 内无路，以及一般模式下吸附到没有出边的节点（行车模式的吸附会跳过这类节点）。
- 远距离规划（同日，`DEC-20261111-1-long-route-01`，组员 1 选“忽略对比，只做 A”）：跨城路线（如武汉→黄州 53.5 km）原先因全量路网超出 0.3°×0.5° / 25 万节点限制而报“工作区域无效或过大”。现在全量不可用或扩大后仍不连通时，仿 Navit `route_depth` 改用分级路网：两端 3 km 全部道路、15 km 内三级路以上、走廊内主干道（超过 100 km 只留高速/国道级）。新增 `ImportedDataset::roadsLayered()`（影响 2 号接口，新增不改旧接口），分级上限为 100 万节点 / 200 万引用；`buildNetwork` 新增上限参数（默认不变）。测试 `longRouteUsesTieredNetwork`；根 CTest 19/19。湖北实测：武昌到黄州/鄂州/孝感/咸宁/荆州/襄阳/宜昌/十堰（53–406 km）两种模式 16/16 成功，0.33–0.74 s，峰值 RSS 281 MiB；分级与全量路线长度比中位 1.0004，但个别可达 2.33 倍（终点只能经 3 km 外小路到达时）。见 `NAVIT-20261111-1-long-route-01`。
- 下一步：2、3、4 号评审；补 Navit 同起终点规划对比；可考虑让一般模式吸附也跳过没有出边/入边的节点；分级路线偏长时，可考虑把“全部道路”半径随路网密度自适应。
- 记录：`DEC-20261111-1-route-mode-01`、`NAVIT-20261111-1-route-mode-01`、AI 日志 22。

### 2026-11-11：兴趣点搜索、“到这去”与吸附虚线

- 负责人：1（用户授权的跨模块修改）；POI 数据与导入缓存主责 2，**需提示 2 号评审**；路线规划沿用既有接口（3、4 号不受影响）。状态：**进行中**。功能、自动化测试与本组性能完成；真实桌面人工操作与 Navit 搜索耗时未完成。
- 能力：右上角搜索按钮（需先有起点）→ 顶部居中搜索栏 → 回车显示离起点最近的 ≤20 个兴趣点（紫色定位针 + 名称前 5 个字，名称互不遮挡）；单击看信息，右键“到这去”设为终点并自动规划；起终点到吸附道路节点画虚线。只有标准 OSM 导入有 POI，OSMX 数据集会提示没有兴趣点数据。
- 代码：`src/data/import/PoiSearch.{h,cpp}`、`PoiSearchCommon.h`、`PoiSearchMemory.cpp`（`searchNearestPois()`：首次搜索时建内存单字倒排，按缓存文件版本在进程内复用；不修改导入缓存）；`src/ui/map/MapView.cpp`（`poiSearchButton/Bar/Input/Close`、`poiInfo`、`poiMenu`/`poiGoHere`；POI 用渲染器 `MarkerKind::PointOfInterest`；虚线在渲染后叠加，并从 A/B 标记边缘起画）。
- 方案：`DEC-20261111-1-poi-index-01`，试验 A FTS5 / B 内存倒排 / C R 树扩圈，选 B，A/C 已清理。
- 同名临近 POI 合并（同日补充，`DEC-20261111-1-poi-dedup-01`）：`src/data/import/PoiDedup.{h,cpp}`，在导入事务内删除同名且距已保留点 ≤ 100 m 的 POI（每簇保留最小 id），写入 `ImportStats::poisMerged`、摘要、警告和 metadata `poi_dedup_m`；`ImportTypes.h` 新增 `poisMerged` 字段（影响 2 号）。旧缓存不重写，`searchNearestPois` 与扫描参考实现在内存中套用同一规则。湖北：合并 3,023 个（36,875 → 33,852），康魅路停车场由 4 个点合为 1 个；重新导入 5.56–5.69 s（n=3，此前单次 5.54 s），峰值 RSS 313–317 MiB。测试 `poiDuplicatesMerged`（链式规则、无名不合并、统计与警告、旧缓存回退与扫描一致）。
- 图标：`MapView.cpp` 的 `poiPinPath()` / `poiShortName()`；`MapColors` 新增 `poi`、`poiSelected`；点击区域为针头与针尖；POI 不再交给渲染器的圆点 marker。
- 测试：`p1-import-tests` 的 `poiDuplicatesMerged`、`poiIndexMatchesFullScan`（1500 个种子 POI，9 个查询 × 2 个 limit 与全表扫描逐 id 一致；只建一次索引；取消；`findPois` 仍可用）；业务测试 `poiSearchGoHere`（按钮位置、无起点提示、搜索栏居中、20 个且与扫描一致、单击信息框、右键菜单在点旁、“到这去”自动规划、虚线像素有实有空、关闭清空）。根 CTest 19/19。
- 湖北实测（`NAVIT-20261111-1-poi-index-01`）：B 的所有查询中位 0.003–0.31 ms（全表扫描约 5 ms），首次建索引 11–14 ms，峰值 RSS 16.6 MiB。
- 限制：索引不落盘，每次启动后首次搜索重建；“名称包含”不做分词与拼音；面状 POI（路/多边形）的位置是外包框中心，凹形时可能落在面外；起终点吸附仍是最近节点（不是最近道路上的投影点），虚线连到该节点；没在真实桌面人工操作过；offscreen 测试屏宽 800 px，菜单位置只在屏内的点上验证。
- 下一步：2 号评审 POI 接口；真实桌面目测；补 Navit 搜索对比。
- 记录：`DEC-20261111-1-poi-index-01`、`NAVIT-20261111-1-poi-index-01`、AI 日志 23。
