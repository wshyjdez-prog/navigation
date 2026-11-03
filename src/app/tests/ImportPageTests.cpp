#include "app/ImportFactory.h"
#include "app/ImportPage.h"
#include "app/ImportTypes.h"
#include "data/osmx/OsmxCache.h"
#include "data/osmx/OsmxStore.h"
#include "ui/common/Theme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <cstring>

namespace {

// Minimal OSMX v1 writer. The page test needs a real file, not a parser mock, so
// it exercises OsmxParser, OsmxCache and OsmxStore through the actual page.
void put32(QByteArray& out, quint32 value) {
    for (int i = 0; i < 4; ++i) out.append(static_cast<char>((value >> (i * 8)) & 0xFF));
}

void put64(QByteArray& out, quint64 value) {
    for (int i = 0; i < 8; ++i) out.append(static_cast<char>((value >> (i * 8)) & 0xFF));
}

void putDouble(QByteArray& out, double value) {
    quint64 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    put64(out, bits);
}

void putString(QByteArray& out, const QString& text) {
    put32(out, static_cast<quint32>(text.size() * 2));
    for (const QChar ch : text) {
        const quint16 unit = ch.unicode();
        out.append(static_cast<char>(unit & 0xFF));
        out.append(static_cast<char>((unit >> 8) & 0xFF));
    }
}

struct WayTag {
    QString key;
    QString value;
};

// Three nodes on a north-south line, one two-way residential way (2 segments,
// 4 directed edges) with a Chinese display name.
QByteArray buildMinimalOsmx() {
    struct Node {
        qint64 id;
        double lat;
        double lon;
    };
    const Node nodes[] = {{1001, 30.5000, 114.3000}, {1002, 30.5010, 114.3000}, {1003, 30.5020, 114.3000}};
    const WayTag tags[] = {{QStringLiteral("highway"), QStringLiteral("residential")},
                           {QStringLiteral("name"), QStringLiteral("测试路")}};

    QByteArray out;
    out.append("OSMX");
    put32(out, 1);      // version
    putString(out, QString());  // header source path (informational, may be empty)
    out.append(static_cast<char>(0x01));  // flags: bounds present
    putDouble(out, 30.5000);
    putDouble(out, 114.3000);
    putDouble(out, 30.5020);
    putDouble(out, 114.3000);
    put64(out, 3);  // nodeCount

    for (const auto& node : nodes) {
        put64(out, static_cast<quint64>(node.id));
        putDouble(out, node.lat);
        putDouble(out, node.lon);
        put32(out, 0);  // tagCount
    }

    put64(out, 1);  // wayCount
    put64(out, 2001);
    put32(out, 3);  // refCount
    for (const auto& node : nodes) put64(out, static_cast<quint64>(node.id));
    put32(out, 2);  // tagCount
    for (const auto& tag : tags) {
        putString(out, tag.key);
        putString(out, tag.value);
    }

    put64(out, 0);  // relationCount
    return out;
}

bool writeAll(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(bytes) == bytes.size();
}

}  // namespace

namespace p1::app {

class ImportPageTests final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void importsOsmxThroughPageAndPublishesSharedDataset();
    void reopensCachedDatasetAfterRestart();

private:
    ImportResult runImport(ImportPage& page, const QString& path);
    QString writeFixture(const QString& name);

    QTemporaryDir* directory_ = nullptr;
    ui::Theme* theme_ = nullptr;
    QString cacheDirectory_;
};

void ImportPageTests::init() {
    directory_ = new QTemporaryDir(QDir::currentPath() + QStringLiteral("/p1-import-page-XXXXXX"));
    QVERIFY(directory_->isValid());
    cacheDirectory_ = directory_->filePath(QStringLiteral("cache"));
    theme_ = new ui::Theme;
    data::OsmxStore::shared().clear();
}

void ImportPageTests::cleanup() {
    data::OsmxStore::shared().clear();
    delete theme_;
    theme_ = nullptr;
    delete directory_;
    directory_ = nullptr;
}

QString ImportPageTests::writeFixture(const QString& name) {
    const QString path = directory_->filePath(name);
    if (!writeAll(path, buildMinimalOsmx())) return QString();
    return path;
}

ImportResult ImportPageTests::runImport(ImportPage& page, const QString& path) {
    QSignalSpy finished(&page, &ImportPage::importFinished);
    page.selectFile(path);
    page.startImport();
    if (finished.isEmpty() && !finished.wait(20000)) return {};
    if (finished.isEmpty()) return {};
    return qvariant_cast<ImportResult>(finished.at(0).at(0));
}

void ImportPageTests::importsOsmxThroughPageAndPublishesSharedDataset() {
    const QString path = writeFixture(QStringLiteral("wuhan-mini.osmx"));
    QVERIFY(!path.isEmpty());

    ImportPage page(*theme_, nullptr, cacheDirectory_);
    const ImportResult result = runImport(page, path);

    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.sourceFormat, QStringLiteral("osmx"));
    QCOMPARE(result.stats.nodes, quint64(3));
    QCOMPARE(result.stats.ways, quint64(1));
    QCOMPARE(result.stats.relations, quint64(0));
    QCOMPARE(result.stats.routableWays, quint64(1));
    QCOMPARE(result.stats.directedEdges, quint64(4));
    QVERIFY(!result.datasetId.isEmpty());
    QVERIFY2(QFile::exists(QDir(result.cachePath).filePath(QStringLiteral("data.osmx"))),
             qPrintable(result.cachePath));
    QVERIFY(QFile::exists(QDir(result.cachePath).filePath(QStringLiteral("manifest.json"))));

    // 3/5 号 read the same immutable instance from the process-wide store.
    const auto dataset = data::OsmxStore::shared().active();
    QVERIFY(dataset);
    QCOMPARE(dataset->nodeCount(), quint32(3));
    QCOMPARE(dataset->wayCount(), quint32(1));
    QCOMPARE(dataset->info().routableWayCount, quint32(1));
    QCOMPARE(dataset->info().directedEdgeCount, quint32(4));
    QCOMPARE(dataset->displayName(0), QStringLiteral("测试路"));
    QCOMPARE(dataset->outgoingEdgeCount(0), quint32(1));
    QCOMPARE(dataset->outgoingEdgeCount(1), quint32(2));

    // The catalog of the same page lists the dataset that was just published.
    // An earlier empty scan may already have placed the placeholder row, so wait
    // for the row text instead of only the row count.
    auto* list = page.findChild<QListWidget*>(QStringLiteral("importedDatasets"));
    QVERIFY(list);
    QTRY_VERIFY_WITH_TIMEOUT(list->count() == 1 && list->item(0)->text().contains(QStringLiteral("wuhan-mini.osmx")),
                             10000);
}

void ImportPageTests::reopensCachedDatasetAfterRestart() {
    const QString path = writeFixture(QStringLiteral("wuhan-mini.osmx"));
    QVERIFY(!path.isEmpty());

    ImportPage first(*theme_, nullptr, cacheDirectory_);
    const ImportResult imported = runImport(first, path);
    QVERIFY2(imported.ok, qPrintable(imported.error));
    const QString datasetId = imported.datasetId;
    const QString importedAt = imported.importedAt;
    QVERIFY(!datasetId.isEmpty());

    // Simulate an application restart: the in-memory store is empty, only the
    // local cache survives.
    data::OsmxStore::shared().clear();
    QVERIFY(!data::OsmxStore::shared().active());

    ImportPage second(*theme_, nullptr, cacheDirectory_);
    auto* list = second.findChild<QListWidget*>(QStringLiteral("importedDatasets"));
    QVERIFY(list);
    QTRY_VERIFY_WITH_TIMEOUT(list->count() == 1 && list->item(0)->text().contains(QStringLiteral("wuhan-mini.osmx")),
                             10000);
    list->setCurrentRow(0);

    auto* open = second.findChild<QPushButton*>(QStringLiteral("openDatasetButton"));
    QVERIFY(open);
    QVERIFY2(open->isEnabled(), qPrintable(open->toolTip()));

    QSignalSpy finished(&second, &ImportPage::importFinished);
    open->click();
    QVERIFY(second.isImporting());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 20000);
    const ImportResult reopened = qvariant_cast<ImportResult>(finished.at(0).at(0));

    QVERIFY2(reopened.ok, qPrintable(reopened.error));
    QCOMPARE(reopened.datasetId, datasetId);
    QCOMPARE(reopened.importedAt, importedAt);
    QCOMPARE(reopened.stats.nodes, quint64(3));

    const auto dataset = data::OsmxStore::shared().active();
    QVERIFY(dataset);
    QCOMPARE(dataset->info().datasetId, datasetId);
    QCOMPARE(dataset->info().importedAt, importedAt);
    QCOMPARE(dataset->nodeCount(), quint32(3));
    QCOMPARE(dataset->info().directedEdgeCount, quint32(4));
}

}  // namespace p1::app

QTEST_MAIN(p1::app::ImportPageTests)
#include "ImportPageTests.moc"
