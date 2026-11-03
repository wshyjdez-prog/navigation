// Command line checker used to validate a real OSMX file outside the GUI.
// It prints the same values the import page receives and exits non-zero on any
// validation error. It is a development/diagnostic tool, not part of the app.

#include "data/osmx/OsmxParser.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTextStream>

#include <cstdio>

using namespace p1::data;

namespace {

void print(const QString& line) {
    const QByteArray bytes = line.toUtf8();
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
    std::fputc('\n', stdout);
}

QString boundsText(const OsmxBounds& bounds) {
    if (!bounds.valid) return QStringLiteral("(none)");
    return QStringLiteral("%1,%2 - %3,%4")
        .arg(bounds.minLat, 0, 'f', 6)
        .arg(bounds.minLon, 0, 'f', 6)
        .arg(bounds.maxLat, 0, 'f', 6)
        .arg(bounds.maxLon, 0, 'f', 6);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTextStream err(stderr);

    const QStringList args = app.arguments();
    if (args.size() < 2 || args.at(1) == QLatin1String("--help") || args.at(1) == QLatin1String("-h")) {
        print(QStringLiteral("用法: p1-osmx-cli <file.osmx> [--no-index] [--header-only]"));
        return args.size() < 2 ? 2 : 0;
    }
    const QString path = args.at(1);
    const bool buildIndex = !args.contains(QStringLiteral("--no-index"));
    const bool headerOnly = args.contains(QStringLiteral("--header-only"));

    if (headerOnly) {
        const OsmxError error = OsmxParser::validateHeader(path);
        if (!error.isOk()) {
            print(QStringLiteral("头校验失败: %1").arg(error.toString()));
            return 1;
        }
        print(QStringLiteral("头校验通过: %1").arg(QFileInfo(path).absoluteFilePath()));
        return 0;
    }

    OsmxParseOptions options;
    options.buildSpatialIndex = buildIndex;
    OsmxParseCallbacks callbacks;
    int lastPercent = -100;
    callbacks.progress = [&lastPercent](int percent, const QString& stage) {
        if (percent / 10 == lastPercent / 10) return;
        lastPercent = percent;
        print(QStringLiteral("[%1%] %2").arg(percent, 3).arg(stage));
    };
    callbacks.log = [](const QString& message) { print(QStringLiteral("日志: %1").arg(message)); };

    QElapsedTimer timer;
    timer.start();
    const OsmxParseResult result = OsmxParser().parse(path, options, callbacks);
    if (!result.ok()) {
        print(QStringLiteral("失败: %1").arg(result.error.toString()));
        if (result.cancelled) print(QStringLiteral("已取消"));
        err << QStringLiteral("耗时 %1 ms\n").arg(result.elapsedMs);
        return 1;
    }

    const auto& dataset = *result.dataset;
    const auto& info = dataset.info();
    print(QStringLiteral("文件: %1").arg(result.sourcePath));
    print(QStringLiteral("大小: %1 字节 (%2 MiB)")
              .arg(info.fileBytes)
              .arg(QString::number(static_cast<double>(info.fileBytes) / (1024.0 * 1024.0), 'f', 2)));
    print(QStringLiteral("SHA-256: %1").arg(info.sourceSha256));
    print(QStringLiteral("版本: OSMX v%1").arg(info.formatVersion));
    print(QStringLiteral("边界: %1").arg(boundsText(info.bounds)));
    print(QStringLiteral("节点: %1  标签: %2").arg(dataset.nodeCount()).arg(info.nodeTagCount));
    print(QStringLiteral("道路: %1  标签: %2  节点引用: %3")
              .arg(dataset.wayCount())
              .arg(info.wayTagCount)
              .arg(info.wayNodeReferenceCount));
    print(QStringLiteral("关系: %1  标签: %2  成员: %3")
              .arg(dataset.relationCount())
              .arg(info.relationTagCount)
              .arg(info.relationMemberCount));
    print(QStringLiteral("可导航道路: %1  无向段: %2  有向边: %3  道路里程: %4 km")
              .arg(info.routableWayCount)
              .arg(info.undirectedSegmentCount)
              .arg(info.directedEdgeCount)
              .arg(info.roadLengthMeters / 1000.0, 0, 'f', 3));
    print(QStringLiteral("空间索引: %1").arg(dataset.hasSpatialIndex() ? QStringLiteral("已构建") : QStringLiteral("未构建")));
    for (const auto& warning : result.warnings) print(QStringLiteral("警告: %1").arg(warning));
    print(QStringLiteral("解析+建索引耗时: %1 ms").arg(result.elapsedMs));

    if (const auto node = dataset.nearestNode(30.52, 114.36, 500.0)) {
        print(QStringLiteral("最近节点查询(30.52,114.36): #%1 id=%2")
                  .arg(*node)
                  .arg(dataset.nodeAt(*node).id));
    }
    return 0;
}
