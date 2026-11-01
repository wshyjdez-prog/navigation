# 标准 OSM 导入（组员 1 授权跨模块装配）

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
