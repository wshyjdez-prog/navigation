# NAVIT-20261101-1-osm-import-01

日期：2026-11-01；负责人：1（用户授权导入替换，2 号后续维护）。状态：已比较（各自预处理成本，限制见下文）。关联 DEC-20261101-1-import-backend-01。

本组源码 main/ca12fe6＋未提交修改，libosmium 2.20.0、protozero 1.7.1、SQLite 缓存；统一 Ubuntu 全工程 Debug/Release 构建通过，CTest 均 7/7，导入专项 17/17、UI 12/12。Navit 为已有便携部署 0.5.6，Ubuntu 包 0.5.6+dfsg.1-2build4；运行时仍须重新核对配置和依赖，不把早前地图转换当成本次基线测量。

计划使用同一 VM、同源湖北 PBF（56,133,820 bytes；SHA256 b099af20e3e02e13a68ec958ae2a5f05ec28f21664dede7eb33f76b20eca0ffe）。宿主 i7-13620H/32GB，VM 1 vCPU/8GiB，I 盘；执行前重新采集系统/工具链、后台负载与磁盘余量。旧地图转换结果不计入本次样本。

功能映射：P1 为原始 OSM 节点/道路/关系持久化，Navit maptool 为导航 bin 预处理，输出语义和过滤存在差异。先核查对象/引用、坐标与标签正确性，再测各自预处理成本；不直接计算性能优劣倍数，不将原始缓存完成说成可导航地图完成。

预定方法：Release 构建；同一 VM 分开执行，热文件缓存；各预热1次、正式3次，固定先 P1 后 Navit，各次独立输出，超时600秒。用 /usr/bin/time 记录进程启动至退出的 wall time 与峰值 RSS（KiB 转 MiB），成功样本报告中位数/P95（nearest-rank，n=3 分位数不稳定）、输入 MiB/s 与输出大小；保留所有错误与超时。P1 内部 elapsedMs 不含最后提交/发布开销，不用于端到端比较。Navit 使用 --protobuf --threads 1 --slice-size 268435456 --unknown-country，需在正式执行前核对版本能力。

| 项目 | P1 | Navit | 差值/相对变化 |
| --- | --- | --- | --- |
| 正确性 | 7/7 CTest、湖北缓存完整性 ok | maptool 各次退出 0，语义质量有限核查 | 不计算 |
| 耗时中位数/P95、峰值 RSS、吞吐量 | 已测，见下表 | 已测，见下表 | 不计算 |

过程限制：VM 曾黑屏、SSH 超时，用户恢复桌面后运行验证已继续。Release 配置首次因无 g++ 默认别名失败；干净构建目录显式使用 /usr/bin/g++-13 后通过。正式样本期间 snap-store 约占 30% CPU、load average 达 4.60（1 vCPU），P1 第二个正式样本升至 215.78 秒，其余约 40 秒；慢样本保留，负载干扰证据见 load-during.txt。本轮仅报告当前环境观察，不能支持稳定性能优劣结论。

## 本轮验收结果（2026-11-01）

状态：**已比较（仅各自预处理成本；语义不同，不计算优劣比例）**。正式样本均 3/3 成功，另各 1 次预热成功，无超时。方法及原始逐次结果分别见同名目录中的 measure.py、evidence/raw.json、summary.json、*.time、*.stdout、*.stderr；环境见 environment.json、load-during.txt，Navit 配置/二进制校验见 navit-hashes.json，61 个源码文件校验一致见 source-manifest.json 与 source-check.txt。

| 指标 | P1 原始 OSM 缓存 | Navit 导航 bin | 差值/相对变化 |
| --- | --- | --- | --- |
| 正式样本耗时（秒） | 40.11 / 215.78 / 41.28 | 49.63 / 47.35 / 46.63 | 不计算：语义不同且有负载干扰 |
| 中位数 / P95（秒，n=3） | 41.28 / 215.78 | 47.35 / 49.63 | 不计算 |
| 三次峰值 RSS 最大值（MiB） | 101.00 | 179.13 | 不计算 |
| 中位输入吞吐量（MiB/s） | 1.297 | 1.131 | 不计算 |
| 输出大小（bytes） | 545054720 | 54703657 | 输出结构不同 |

P1 正确性：CTest 7/7，导入专项 17/17、UI offscreen/Wayland 各 12/12；真实湖北数据 SQLite integrity_check=ok，节点 9077243、道路 593776、关系 5262、有序道路节点引用 10051981、关系成员 237160、标签 1789851。读取元数据与实表统计一致；页面重启恢复并截图。缺失关系节点 1293、关系道路 141198、关系成员 1719，原引用保留且页面标注警告；道路节点引用检查未报缺失。该事实不保证路由图可用。

Navit 正确性边界：本轮 maptool 全部退出 0、各阶段完成，输出大小一致；原始 stderr 保留缺失引用/转换警告。未逐对象比较 Navit bin 与原始 OSM（其本来会过滤/转码），未以本轮转换验证完整导航质量。因此只报告各自成本，不得将耗时表当成等价质量下领先证据。建议后台负载稳定后重测，并由 2/3/5 号共同定义“可供导航的同源数据准备”及质量标准。

安装与界面：/home/navigation/p1-import-libosmium-20261101/install/bin/p1-navigation；桌面和应用菜单既有 P1 快捷方式已更新，应用已启动；系统依赖及用户 SDK 中的版权说明随 install/share/p1-navigation/licenses 保存。截图见 evidence/captures/，安装过程及真实 Wayland 用例见 install-verification.txt、wayland-ui.txt。数据仍在 I 盘承载的 VM 内，未装到宿主 C 盘。
