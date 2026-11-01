#include "data/import/OsmImportParser.h"
#include "contracts/ImportedDataset.h"
#include <osmium/io/any_input.hpp>
#include <osmium/io/any_output.hpp>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

namespace {
QByteArray xml() {
    return "<?xml version=\"1.0\"?><osm version=\"0.6\" generator=\"P1 test\">"
      "<node id=\"1\" lat=\"30.5\" lon=\"114.3\"><tag k=\"name\" v=\"测试地点\"/></node>"
      "<node id=\"2\" lat=\"30.6\" lon=\"114.4\"/>"
      "<way id=\"10\"><nd ref=\"1\"/><nd ref=\"2\"/><tag k=\"highway\" v=\"residential\"/></way>"
      "<relation id=\"20\"><member type=\"way\" ref=\"10\" role=\"outer\"/><tag k=\"type\" v=\"route\"/></relation></osm>";
}
void write(const QString& path,const QByteArray& bytes) {
    QFile f(path);
    if(!f.open(QIODevice::WriteOnly) || f.write(bytes)!=bytes.size()) throw std::runtime_error("fixture write failed");
}
}
class ImportTests : public QObject {
    Q_OBJECT
private slots:
    void formats_data() {
        QTest::addColumn<QString>("extension");
        for(const auto& e:{".osm",".pbf",".osm.pbf",".osm.gz",".osm.bz2"}) QTest::newRow(e)<<QString(e);
    }
    void formats() {
        QFETCH(QString,extension);
        QTemporaryDir dir;
        const auto source=dir.filePath("source.osm"), target=dir.filePath("input"+extension);
        write(source,xml());
        {
            osmium::io::Reader reader(source.toStdString());
            osmium::io::Writer writer(target.toStdString(),osmium::io::overwrite::allow);
            while(auto buffer=reader.read()) writer(std::move(buffer));
            writer.close(); reader.close();
        }
        p1::data::OsmImportParser parser(dir.filePath("cache"));
        QVector<int> progress;
        const auto r=parser.parse(target,[&](int p,const QString&){progress.append(p);},{},{});
        QVERIFY2(r.ok,qPrintable(r.error)); QVERIFY(r.warnings.isEmpty());
        QCOMPARE(r.stats.nodes,quint64(2)); QCOMPARE(r.stats.ways,quint64(1));
        QCOMPARE(r.stats.relations,quint64(1)); QCOMPARE(r.stats.tags,quint64(3));
        QCOMPARE(r.stats.nodeReferences,quint64(2)); QCOMPARE(r.stats.relationMembers,quint64(1));
        QCOMPARE(progress.last(),100);
        QVERIFY(std::is_sorted(progress.cbegin(),progress.cend()));
        p1::data::ImportedDataset data(r.cachePath);
        const auto n=data.node(1); QVERIFY(n.has_value()); QCOMPARE(n->latitude,30.5);
        QCOMPARE(n->tags[0].second,QString("测试地点"));
        QVERIFY(!data.node(999).has_value());
        QCOMPARE(data.way(10)->nodes,QVector<qint64>({1,2}));
        QCOMPARE(data.relation(20)->members[0].role,QString("outer"));
        QCOMPARE(data.nodes(0,1).size(),1);
        QVERIFY_EXCEPTION_THROWN(data.nodes(0,1001),std::invalid_argument);
        QCOMPARE(data.summary().sourceSha256,r.sourceSha256);
        QCOMPARE(data.summary().stats.tags,r.stats.tags);
        QCOMPARE(p1::data::listImportedDatasets(dir.filePath("cache")).size(),1);
    }
    void invalidInputs_data() {
        QTest::addColumn<QByteArray>("input"); QTest::addColumn<QString>("suffix");
        QTest::newRow("empty")<<QByteArray()<<QString(".osm");
        QTest::newRow("truncated")<<xml().left(60)<<QString(".osm");
        QTest::newRow("duplicate-id")<<QByteArray("<osm version=\"0.6\"><node id=\"1\" lat=\"30\" lon=\"114\"/><node id=\"1\" lat=\"31\" lon=\"115\"/></osm>")<<QString(".osm");
        QTest::newRow("latitude")<<QByteArray("<osm version=\"0.6\"><node id=\"1\" lat=\"91\" lon=\"114\"/></osm>")<<QString(".osm");
        QTest::newRow("no-location")<<QByteArray("<osm version=\"0.6\"><node id=\"1\"/></osm>")<<QString(".osm");
        QTest::newRow("deleted")<<QByteArray("<osm version=\"0.6\"><node id=\"1\" visible=\"false\"/></osm>")<<QString(".osm");
        QTest::newRow("broken-pbf")<<QByteArray("not a pbf")<<QString(".pbf");
        QTest::newRow("unsupported")<<xml()<<QString(".osmx");
    }
    void invalidInputs() {
        QFETCH(QByteArray,input); QFETCH(QString,suffix);
        QTemporaryDir dir; const auto source=dir.filePath("source"+suffix); write(source,input);
        p1::data::OsmImportParser parser(dir.filePath("cache"));
        const auto r=parser.parse(source,{},{},{});
        QVERIFY(!r.ok); QVERIFY(!r.error.isEmpty()); QVERIFY(r.cachePath.isEmpty());
        QCOMPARE(p1::data::listImportedDatasets(dir.filePath("cache")).size(),0);
    }
    void cancellationAndRollback() {
        QTemporaryDir dir; const auto source=dir.filePath("source.osm"); write(source,xml());
        p1::data::OsmImportParser parser(dir.filePath("cache"));
        const auto first=parser.parse(source,{},{},{}); QVERIFY(first.ok);
        const auto pre=parser.parse(source,{},{},[]{return true;}); QVERIFY(pre.cancelled);
        bool cancel=false;
        const auto mid=parser.parse(source,[&](int p,const QString&){if(p>=10) cancel=true;},{},[&]{return cancel;});
        QVERIFY(mid.cancelled); QVERIFY(!mid.ok);
        write(source,xml().left(50)); QVERIFY(!parser.parse(source,{},{},{}).ok);
        QCOMPARE(p1::data::listImportedDatasets(dir.filePath("cache")).size(),1);
        QVERIFY(p1::data::ImportedDataset(first.cachePath).node(1).has_value());
        QVERIFY(QDir(dir.filePath("cache")).entryList({".import-*"},QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot).isEmpty());
    }
    void incompleteExtractIsReported() {
        QTemporaryDir dir; const auto source=dir.filePath("source.osm");
        write(source,"<osm version=\"0.6\"><way id=\"10\"><nd ref=\"999\"/></way></osm>");
        p1::data::OsmImportParser parser(dir.filePath("cache"));
        const auto r=parser.parse(source,{},{},{});
        QVERIFY2(r.ok,qPrintable(r.error)); QCOMPARE(r.warnings.size(),1);
        QCOMPARE(p1::data::ImportedDataset(r.cachePath).way(10)->nodes,QVector<qint64>({999}));
        QCOMPARE(p1::data::ImportedDataset(r.cachePath).summary().warnings,r.warnings);
    }
};
QTEST_GUILESS_MAIN(ImportTests)
#include "ImportTests.moc"
