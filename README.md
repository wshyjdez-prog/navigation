# P1 Linux 导航项目

**组名：Linux 端导航应用开发组（Linux PAL 适配）。**

5 人使用 C++、Qt Widgets / OpenGL ES 和 CMake 开发 Linux 导航应用，集成 A*/HMM，完成渲染、语音、存储、定位四项 PAL 适配，并与 G6 联调。当前已建立第一次作业 Qt Widgets 应用框架源码，Linux 构建与桌面验收状态见 [框架说明](docs/assignment-1-framework.md)；标准 OSM 导入已接入；其他业务模块尚待集成。

## 第一次作业：应用框架

根目录 `CMakeLists.txt` 是 Qt Creator 工程入口。窗口、页面导航、明暗主题、文件选择占位页、公共组件演示和模块页面替换已实现；详细构建步骤、关键函数与 2/3/4/5 号交接入口见 [基础框架说明](docs/assignment-1-framework.md)。标准 OSM 导入已由 1 号授权装配，支持 PBF、OSM XML、gzip/bzip2 压缩，后台解析并保存 SQLite 缓存，详见 [导入说明](docs/osm-import.md)。

在团队 Ubuntu 虚拟机的工程目录中执行：

```bash
bash tools/verify-environment.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/g++-13
cmake --build build -j1
ctest --test-dir build --output-on-failure
./build/p1-navigation
```

## 第二次作业：应用集成

团队整合版将 2 号 OSMX 源码、3 号 Boost A*/FMM 适配层、4 号异步规划桥和 5 号 LinuxRenderer 纳入同一根工程；导入页保留标准 OSM/PBF 与 OSMX 两条真实路径，地图页仍明确标注 Mock。默认开启真实 A* 对接与 FMM 适配层构建，需补齐 Boost 头文件；真实 HMM 运行另需配置外部 FMM 程序、网络和 UBODT。构建方法、验证证据及未贯通能力见 [团队整合说明](docs/assignment-2-team-integration.md)。算法调用页面、监控及真实数据到算法/地图的转换仍待完成，不把共同构建通过等同完整作业验收。

## 统一开发环境

五人统一在 **Linux 桌面虚拟机**中开发和验证：Ubuntu Desktop **24.04 LTS（noble，amd64）**、Qt **6.4.2**、GCC/G++ **13.3.0**、CMake **3.28.3**（C++17）。安装、版本核对与复现步骤见 [统一开发环境](docs/development-environment.md)。1 号本机已完成虚拟机安装、版本核对、Qt 桌面窗口与 Qt Creator 构建验证；其他成员待复现，见 [实测记录](docs/environment/member-1-2026-10-26.md)。

## 团队分工

| 组员 | 主要职责 |
| --- | --- |
| 1 | Qt 框架/公共 UI、CMake 与发布、用户页面、LinuxVoiceAdapter（TTS/ASR/FFI） |
| 2 | 导入、StorageAdapter（SQLite/文件）、离线包、用户本地数据 |
| 3 | C++ A*/HMM 集成、异步任务、算法调用页与性能监控 |
| 4 | LocationAdapter（GeoClue2/权限）、导航/偏航/模拟回放、G6 联调报告 |
| 5 | LinuxRenderer（Qt Widget/OpenGL ES）、地图交互与仪表盘 |

## 提示词框架

UI 设计与实现统一遵循 [design.md](design.md)：包含 Clay 风格明暗配色、字体、留白、组件样式、地图工作台及第一次作业数据导入页面规范。涉及 UI 时先读此文档；当前公共主题与应用外壳已按规范编写，业务导入已接入，地图和导航仍待接入。

新对话可以使用 [prompts/README.md](prompts/README.md)：按“通用开场 + 本人职责 + 本次流程”组合，覆盖五人模块开发、算法方案对比、跨组联调、排错与验收。

例如：`我是 P1 Linux 组员 2，请使用 prompts/start.md、members.md 的 2 号职责和 workflows.md 的模块开发流程，处理本次数据导入任务。`

## 开始工作

1. 阅读 [当前任务书摘录](docs/linux-assignment-reference.md) 与 [project.md](project.md)，确认环境、职责、验收和跨组接口。
2. 阅读 [AGENTS.md](AGENTS.md)，向 AI 说明组员编号和具体任务；查看 [AI 历史日志](ai_log.md) 与 [方案抉择记录](decision_log.md)。
3. 各模块先使用公共契约和固定样例独立开发，完成后联调；算法方案试验与决策按记录模板保留证据。

推荐开场：`我是 P1 Linux 组员 3。请阅读项目规划、协作规则和方案抉择记录，再处理 C++ 算法集成任务。`

支持 AGENTS.md 的工具应读取根目录规则；其他工具请手动附加相关文件。构建步骤与实际验证状态见基础框架说明，未完成的验收不得按示例命令视为通过。最终产物为 Linux 可执行程序及完整 CMake 工程，附运行截图、3–5 分钟 Linux 视频与 G6 联调报告。

旧平台任务摘录仅供历史参考，当前 P1 范围以新 PDF 为准。
