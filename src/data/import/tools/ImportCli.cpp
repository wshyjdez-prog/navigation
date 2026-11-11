#include "data/import/OsmImportParser.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    QCoreApplication::setApplicationName("p1-navigation");
    QCoreApplication::setOrganizationName("P1Linux");
    if(argc<2 || argc>3) {std::fprintf(stderr,"Usage: p1-import-cli INPUT [CACHE_DIRECTORY]\n"); return 2;}
    p1::data::OsmImportParser parser(argc==3?QString::fromLocal8Bit(argv[2]):QString());
    const auto r=parser.parse(QString::fromLocal8Bit(argv[1]),{},{},{});
    const QJsonObject result{{"ok",r.ok},{"error",r.error},{"cachePath",r.cachePath},
        {"nodes",QString::number(r.stats.nodes)},{"ways",QString::number(r.stats.ways)},
        {"relations",QString::number(r.stats.relations)},
        {"pois",QString::number(r.stats.pois)},{"roads",QString::number(r.stats.roads)},
        {"areas",QString::number(r.stats.areas)},{"areasSkipped",QString::number(r.stats.areasSkipped)},
        {"elapsedMs",QString::number(r.elapsedMs)},{"warnings",r.warnings.size()}};
    std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    return r.ok?0:1;
}
