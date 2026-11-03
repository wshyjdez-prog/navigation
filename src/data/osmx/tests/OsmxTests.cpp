#include "data/osmx/OsmxCache.h"
#include "data/osmx/OsmxParser.h"
#include "data/osmx/OsmxStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace p1::data;

namespace {

struct TagSpec {
    QString key;
    QString value;
};

struct NodeSpec {
    qint64 id = 0;
    double lat = 0.0;
    double lon = 0.0;
    QVector<TagSpec> tags;
};

struct WaySpec {
    qint64 id = 0;
    QVector<qint64> refs;
    QVector<TagSpec> tags;
};

struct MemberSpec {
    quint32 type = 1;
    qint64 ref = 0;
    QString role;
};

struct RelationSpec {
    qint64 id = 0;
    QVector<MemberSpec> members;
    QVector<TagSpec> tags;
};

struct Fixture {
    QVector<NodeSpec> nodes;
    QVector<WaySpec> ways;
    QVector<RelationSpec> relations;
    quint32 version = kOsmxVersion;
    char magic = 'X';
    bool includeBounds = true;
    bool badCoordinate = false;
    bool badReference = false;
    bool oddString = false;
    bool unknownFlags = false;
    QByteArray trailing;
};

QByteArray buildFixture(Fixture fixture) {
    QByteArray out;
    auto put = [&out](char c) { out.append(c); };
    auto put8 = [&put](quint8 v) { put(static_cast<char>(v)); };
    auto put32 = [&put](quint32 v) {
        for (int i = 0; i < 4; ++i) put(static_cast<char>((v >> (i * 8)) & 0xFF));
    };
    auto put64 = [&put](quint64 v) {
        for (int i = 0; i < 8; ++i) put(static_cast<char>((v >> (i * 8)) & 0xFF));
    };
    auto putDouble = [&put64](double value) {
        quint64 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        put64(bits);
    };
    auto putString = [&out](const QString& value, bool odd = false) {
        const QByteArray utf16(reinterpret_cast<const char*>(value.utf16()),
                               static_cast<qsizetype>(value.size() * 2));
        quint32 length = static_cast<quint32>(utf16.size());
        if (odd) length |= 1u;
        for (int i = 0; i < 4; ++i) out.append(static_cast<char>((length >> (i * 8)) & 0xFF));
        out.append(utf16);
    };
    auto putTags = [&put32, &putString](const QVector<TagSpec>& tags, bool oddFirst) {
        put32(static_cast<quint32>(tags.size()));
        bool first = true;
        for (const auto& tag : tags) {
            putString(tag.key, oddFirst && first);
            putString(tag.value, false);
            first = false;
        }
    };

    // "OSMX" magic; the bad-magic fixture changes the last byte only.
    out.append("OSM");
    put(fixture.magic);
    put32(fixture.version);
    putString(QStringLiteral("C:/fixtures/synthetic.osmx"));
    put8(static_cast<quint8>((fixture.includeBounds ? 0x01 : 0x00) | (fixture.unknownFlags ? 0x02 : 0x00)));
    double minLat = 0.0;
    double minLon = 0.0;
    double maxLat = 0.0;
    double maxLon = 0.0;
    bool started = false;
    for (const auto& node : fixture.nodes) {
        if (!started) {
            minLat = maxLat = node.lat;
            minLon = maxLon = node.lon;
            started = true;
        } else {
            minLat = std::min(minLat, node.lat);
            maxLat = std::max(maxLat, node.lat);
            minLon = std::min(minLon, node.lon);
            maxLon = std::max(maxLon, node.lon);
        }
    }
    if (fixture.includeBounds) {
        putDouble(minLat);
        putDouble(minLon);
        putDouble(maxLat);
        putDouble(maxLon);
    }
    put64(static_cast<quint64>(fixture.nodes.size()));
    for (int i = 0; i < fixture.nodes.size(); ++i) {
        const auto& node = fixture.nodes.at(i);
        put64(static_cast<quint64>(node.id));
        putDouble(fixture.badCoordinate && i == 0 ? 100.0 : node.lat);
        putDouble(node.lon);
        putTags(node.tags, fixture.oddString && i == 0);
    }
    put64(static_cast<quint64>(fixture.ways.size()));
    for (const auto& way : fixture.ways) {
        put64(static_cast<quint64>(way.id));
        put32(static_cast<quint32>(way.refs.size()));
        for (const qint64 ref : way.refs) {
            put64(static_cast<quint64>(fixture.badReference ? 999999 : ref));
        }
        putTags(way.tags, false);
    }
    put64(static_cast<quint64>(fixture.relations.size()));
    for (const auto& relation : fixture.relations) {
        put64(static_cast<quint64>(relation.id));
        put32(static_cast<quint32>(relation.members.size()));
        for (const auto& member : relation.members) {
            put32(member.type);
            put64(static_cast<quint64>(member.ref));
            putString(member.role);
        }
        putTags(relation.tags, false);
    }
    out.append(fixture.trailing);
    return out;
}

Fixture syntheticFixture() {
    Fixture fixture;
    fixture.nodes = {
        NodeSpec{1, 30.5000, 114.3000, {{QStringLiteral("amenity"), QStringLiteral("bus_station")},
                                        {QStringLiteral("name"), QStringLiteral("测试站")}}},
        NodeSpec{2, 30.5010, 114.3000, {}},
        NodeSpec{3, 30.5020, 114.3000, {}},
        NodeSpec{4, 30.5000, 114.3010, {}},
        NodeSpec{5, 30.5030, 114.3040, {}},
    };
    fixture.ways = {
        WaySpec{100,
                {1, 2, 3},
                {{QStringLiteral("highway"), QStringLiteral("residential")},
                 {QStringLiteral("name"), QStringLiteral("Luoyu Road")},
                 {QStringLiteral("name:zh"), QStringLiteral("珞喻路")},
                 {QStringLiteral("lanes"), QStringLiteral("2")},
                 {QStringLiteral("maxspeed"), QStringLiteral("30")},
                 {QStringLiteral("oneway"), QStringLiteral("yes")}}},
        WaySpec{101,
                {1, 4},
                {{QStringLiteral("highway"), QStringLiteral("footway")},
                 {QStringLiteral("foot"), QStringLiteral("yes")}}},
        WaySpec{102,
                {2, 4},
                {{QStringLiteral("highway"), QStringLiteral("secondary")},
                 {QStringLiteral("oneway"), QStringLiteral("-1")},
                 {QStringLiteral("bridge"), QStringLiteral("yes")},
                 {QStringLiteral("maxspeed"), QStringLiteral("40 km/h")}}},
        WaySpec{103,
                {1, 2, 3, 1},
                {{QStringLiteral("building"), QStringLiteral("yes")},
                 {QStringLiteral("name"), QStringLiteral("测试楼")}}},
        WaySpec{104, {3}, {{QStringLiteral("highway"), QStringLiteral("service")}}},
    };
    fixture.relations = {
        RelationSpec{1000,
                     {{1, 100, QStringLiteral("outer")},
                      {0, 1, QStringLiteral("stop")},
                      {2, 1001, QString()}},
                     {{QStringLiteral("type"), QStringLiteral("route")},
                      {QStringLiteral("name"), QStringLiteral("测试线")}}},
        RelationSpec{1001, {}, {}},
    };
    return fixture;
}

QString writeFixture(const QByteArray& bytes, QTemporaryDir& directory, const QString& name) {
    const QString path = directory.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return QString();
    if (file.write(bytes) != bytes.size()) return QString();
    file.close();
    return path;
}

OsmxParseResult parseFile(const QString& path, const OsmxParseCallbacks& callbacks = {},
                          const QString& datasetId = QStringLiteral("synthetic-dataset")) {
    OsmxParseOptions options;
    options.datasetId = datasetId;
    return OsmxParser().parse(path, options, callbacks);
}

} // namespace

class OsmxTests : public QObject {
    Q_OBJECT
private slots:
    void parsesSyntheticNetwork();
    void reportsMonotonicProgress();
    void honoursCancellation();
    void validatesHeader();
    void rejectsMalformedFiles_data();
    void rejectsMalformedFiles();
    void installsAndListsCache();
    void skipsCorruptCacheEntries();
    void storePublishesSharedDataset();
    void parsesRealFileWhenProvided();
};

void OsmxTests::parsesSyntheticNetwork() {
    // Test sandboxes (and some Windows service contexts) deny writes to the
    // system temp directory, so keep fixtures next to the test binary.
    QTemporaryDir directory(QDir::currentPath() + QStringLiteral("/p1-osmx-test-XXXXXX"));
    QVERIFY(directory.isValid());
    const QString path = writeFixture(buildFixture(syntheticFixture()), directory, QStringLiteral("synthetic.osmx"));
    QVERIFY(!path.isEmpty());

    const OsmxParseResult result = parseFile(path);
    QVERIFY2(result.ok(), qPrintable(result.error.toString()));
    const auto& dataset = *result.dataset;
    QCOMPARE(dataset.nodeCount(), 5u);
    QCOMPARE(dataset.wayCount(), 5u);
    QCOMPARE(dataset.relationCount(), 2u);
    QCOMPARE(dataset.info().datasetId, QStringLiteral("synthetic-dataset"));
    QCOMPARE(dataset.info().formatVersion, 1u);
    QCOMPARE(result.sha256.size(), qsizetype(64));
    QVERIFY(dataset.hasSpatialIndex());
    QCOMPARE(dataset.info().nodeTagCount, 2ull);
    QCOMPARE(dataset.info().wayTagCount, 15ull);
    QCOMPARE(dataset.info().relationTagCount, 2ull);
    QCOMPARE(dataset.info().wayNodeReferenceCount, 12ull);
    QCOMPARE(dataset.info().relationMemberCount, 3ull);
    QCOMPARE(dataset.info().routableWayCount, 3u);
    QCOMPARE(dataset.info().directedEdgeCount, 5u);
    QCOMPARE(dataset.edges().size(), qsizetype(5));
    QCOMPARE(result.warnings.size(), qsizetype(1));
    QVERIFY(result.warnings.first().contains(QStringLiteral("几何不足")));

    // IDs and tags.
    QCOMPARE(dataset.nodeAt(*dataset.nodeIndex(1)).lat, 30.5);
    QCOMPARE(dataset.nodeTag(*dataset.nodeIndex(1), u"amenity").value(), QStringLiteral("bus_station"));
    QVERIFY(!dataset.nodeTag(*dataset.nodeIndex(1), u"missing").has_value());
    QCOMPARE(dataset.wayTag(*dataset.wayIndex(100), u"name"), QStringLiteral("Luoyu Road"));
    QCOMPARE(dataset.displayName(*dataset.wayIndex(100)), QStringLiteral("珞喻路"));
    QCOMPARE(dataset.wayNodeIds(*dataset.wayIndex(100)), QVector<qint64>({1, 2, 3}));
    QCOMPARE(dataset.relationMembers(*dataset.relationIndex(1000)).size(), qsizetype(3));

    const auto way100 = *dataset.wayIndex(100);
    const auto& attributes = dataset.wayAt(way100).attributes;
    QCOMPARE(attributes.roadClass, RoadClass::Residential);
    QVERIFY(attributes.isRoutable());
    QVERIFY(attributes.isOneway());
    QVERIFY(!attributes.isOnewayReversed());
    QCOMPARE(attributes.lanes, quint8(2));
    QCOMPARE(attributes.maxSpeedKmh, quint16(30));
    const double expectedLength = osmxDistanceMeters(30.5, 114.3, 30.501, 114.3) +
                                  osmxDistanceMeters(30.501, 114.3, 30.502, 114.3);
    QVERIFY(std::abs(dataset.wayAt(way100).attributes.lengthMeters - expectedLength) < 0.01);

    const auto way102 = *dataset.wayIndex(102);
    QVERIFY(dataset.wayAt(way102).attributes.isOneway());
    QVERIFY(dataset.wayAt(way102).attributes.isOnewayReversed());
    QVERIFY(dataset.wayAt(way102).attributes.isBridge());
    QCOMPARE(dataset.wayAt(way102).attributes.maxSpeedKmh, quint16(40));

    const auto way103 = *dataset.wayIndex(103);
    QVERIFY(dataset.wayAt(way103).attributes.isArea());
    QVERIFY(!dataset.wayAt(way103).attributes.isRoutable());

    const auto way104 = *dataset.wayIndex(104);
    QVERIFY(!dataset.wayAt(way104).attributes.isRoutable());
    QVERIFY(dataset.wayAt(way104).attributes.hasFlag(RoadIncompleteGeometry));

    // Incidence is sorted and deduplicated (way 103 references node 1 twice).
    const QVector<quint32> incident = dataset.incidentWays(*dataset.nodeIndex(2));
    QCOMPARE(incident, QVector<quint32>({0, 2, 3}));

    // Directed edges: 100 forward (1->2, 2->3), 101 both ways (1<->4), 102
    // reversed (4->2). Node 4 (index 3) therefore has 4->1 and 4->2.
    const quint32 node4 = *dataset.nodeIndex(4);
    QCOMPARE(dataset.outgoingEdgeCount(node4), 2u);
    QSet<qint64> targets;
    for (quint32 i = 0; i < dataset.outgoingEdgeCount(node4); ++i) {
        const auto& edge = dataset.edges().at(static_cast<int>(dataset.outgoingEdgeFirst(node4) + i));
        targets.insert(dataset.nodeAt(edge.to).id);
        QVERIFY(dataset.hasWay(edge.way));
    }
    QVERIFY(targets.contains(1));
    QVERIFY(targets.contains(2));

    // Spatial helpers.
    double distance = 0.0;
    const auto nearest = dataset.nearestNode(30.50002, 114.30003, 500.0, &distance);
    QVERIFY(nearest.has_value());
    QCOMPARE(dataset.nodeAt(*nearest).id, qint64(1));
    QVERIFY(distance < 10.0);
    OsmxBounds area{30.4999, 114.2999, 30.5001, 114.3001, true};
    QCOMPARE(dataset.waysInBounds(area).size(), qsizetype(4));
    QCOMPARE(dataset.waysInBounds(area, 0, true).size(), qsizetype(3));
    QCOMPARE(dataset.waysInBounds(area, 1).size(), qsizetype(1));

    // Out-of-range helpers must not crash or read out of bounds.
    QVERIFY(!dataset.hasNode(99));
    QVERIFY(dataset.incidentWays(999).isEmpty());
    QCOMPARE(dataset.outgoingEdgeCount(999), 0u);
}

void OsmxTests::reportsMonotonicProgress() {
    // Test sandboxes (and some Windows service contexts) deny writes to the
    // system temp directory, so keep fixtures next to the test binary.
    QTemporaryDir directory(QDir::currentPath() + QStringLiteral("/p1-osmx-test-XXXXXX"));
    QVERIFY(directory.isValid());
    const QString path = writeFixture(buildFixture(syntheticFixture()), directory, QStringLiteral("progress.osmx"));
    QVERIFY(!path.isEmpty());

    QVector<int> percents;
    QStringList stages;
    OsmxParseCallbacks callbacks;
    callbacks.progress = [&percents, &stages](int percent, const QString& stage) {
        percents.append(percent);
        stages.append(stage);
    };
    const OsmxParseResult result = parseFile(path, callbacks);
    QVERIFY2(result.ok(), qPrintable(result.error.toString()));
    QVERIFY(percents.size() >= 5);
    QCOMPARE(percents.first(), 0);
    QCOMPARE(percents.last(), 100);
    for (int i = 1; i < percents.size(); ++i) QVERIFY(percents.at(i) >= percents.at(i - 1));
    QVERIFY(stages.contains(QStringLiteral("解析节点")));
    QVERIFY(stages.contains(QStringLiteral("解析道路")));
    QVERIFY(stages.contains(QStringLiteral("构建拓扑与空间索引")));
}

void OsmxTests::honoursCancellation() {
    // Test sandboxes (and some Windows service contexts) deny writes to the
    // system temp directory, so keep fixtures next to the test binary.
    QTemporaryDir directory(QDir::currentPath() + QStringLiteral("/p1-osmx-test-XXXXXX"));
    QVERIFY(directory.isValid());
    const QString path = writeFixture(buildFixture(syntheticFixture()), directory, QStringLiteral("cancel.osmx"));
    QVERIFY(!path.isEmpty());

    OsmxParseCallbacks callbacks;
    callbacks.cancelled = [] { return true; };
    const OsmxParseResult result = parseFile(path, callbacks);
    QVERIFY(!result.ok());
    QVERIFY(result.cancelled);
    QCOMPARE(result.error.code, OsmxErrorCode::Cancelled);
    QVERIFY(result.dataset == nullptr);
}

void OsmxTests::validatesHeader() {
    // Test sandboxes (and some Windows service contexts) deny writes to the
    // system temp directory, so keep fixtures next to the test binary.
    QTemporaryDir directory(QDir::currentPath() + QStringLiteral("/p1-osmx-test-XXXXXX"));
    QVERIFY(directory.isValid());
    Fixture fixture = syntheticFixture();
    const QString path = writeFixture(buildFixture(fixture), directory, QStringLiteral("header.osmx"));
    QVERIFY(OsmxParser::validateHeader(path).isOk());
    QVERIFY(OsmxParser::supports(path));
    QVERIFY(!OsmxParser::supports(directory.filePath(QStringLiteral("x.pbf"))));

    fixture.magic = 'Y';
    const QString badPath = writeFixture(buildFixture(fixture), directory, QStringLiteral("bad-header.osmx"));
    QCOMPARE(OsmxParser::validateHeader(badPath).code, OsmxErrorCode::BadMagic);
}

void OsmxTests::rejectsMalformedFiles_data() {
    QTest::addColumn<int>("variant");
    QTest::newRow("bad-magic") << 0;
    QTest::newRow("bad-version") << 1;
    QTest::newRow("no-bounds-flag") << 2;
    QTest::newRow("bad-coordinate") << 3;
    QTest::newRow("missing-way-node") << 4;
    QTest::newRow("duplicate-node") << 5;
    QTest::newRow("odd-string") << 6;
    QTest::newRow("trailing-bytes") << 7;
    QTest::newRow("unknown-flags") << 8;
}

void OsmxTests::rejectsMalformedFiles() {
    QFETCH(int, variant);
    // Test sandboxes (and some Windows service contexts) deny writes to the
    // system temp directory, so keep fixtures next to the test binary.
    QTemporaryDir directory(QDir::currentPath() + QStringLiteral("/p1-osmx-test-XXXXXX"));
    QVERIFY(directory.isValid());
    Fixture fixture = syntheticFixture();
    OsmxErrorCode expected = OsmxErrorCode::None;
    switch (variant) {
    case 0:
        fixture.magic = 'Y';
        expected = OsmxErrorCode::BadMagic;
        break;
    case 1:
        fixture.version = 2;
        expected = OsmxErrorCode::UnsupportedVersion;
        break;
    case 2:
        fixture.includeBounds = false;
        expected = OsmxErrorCode::None; // bounds are optional; must parse
        break;
    case 3:
        fixture.badCoordinate = true;
        expected = OsmxErrorCode::BadCoordinate;
        break;
    case 4:
        fixture.badReference = true;
        expected = OsmxErrorCode::CorruptRecord;
        break;
    case 5:
        // Duplicate the first node record after the node table was written:
        // the builder writes the table from the vector, so append a duplicate
        // spec and keep the declared count in sync.
        fixture.nodes.append(fixture.nodes.first());
        expected = OsmxErrorCode::DuplicateId;
        break;
    case 6:
        fixture.oddString = true;
        expected = OsmxErrorCode::BadString;
        break;
    case 7:
        fixture.trailing = QByteArray(4, '\0');
        expected = OsmxErrorCode::TrailingData;
        break;
    case 8:
        fixture.unknownFlags = true;
        expected = OsmxErrorCode::UnsupportedFlags;
        break;
    default:
        QFAIL("unknown variant");
    }

    const QString path = writeFixture(buildFixture(fixture), directory,
                                      QStringLiteral("case-%1.osmx").arg(variant));
    QVERIFY(!path.isEmpty());
    const OsmxParseResult result = parseFile(path);
    if (expected == OsmxErrorCode::None) {
        QVERIFY2(result.ok(), qPrintable(result.error.toString()));
        QCOMPARE(result.dataset->info().bounds.valid, true);
        QCOMPARE(result.dataset->bounds().minLat, 30.5);
    } else {
        QVERIFY2(!result.ok(), "malformed file was accepted");
        QCOMPARE(result.error.code, expected);
        QVERIFY(!result.error.message.isEmpty());
        QVERIFY(result.error.offset >= 0 || expected == OsmxErrorCode::InvalidDataset);
    }
}

void OsmxTests::installsAndListsCache() {
    // Test sandboxes (and some Windows service contexts) deny writes to the
    // system temp directory, so keep fixtures next to the test binary.
    QTemporaryDir directory(QDir::currentPath() + QStringLiteral("/p1-osmx-test-XXXXXX"));
    QVERIFY(directory.isValid());
    const QString path = writeFixture(buildFixture(syntheticFixture()), directory, QStringLiteral("cached.osmx"));
    const OsmxParseResult parsed = parseFile(path, {}, QStringLiteral("cache-dataset"));
    QVERIFY2(parsed.ok(), qPrintable(parsed.error.toString()));

    const QString cacheRoot = directory.filePath(QStringLiteral("cache"));
    QVector<int> progress;
    OsmxParseCallbacks callbacks;
    callbacks.progress = [&progress](int percent, const QString&) { progress.append(percent); };
    const OsmxInstallResult installed = OsmxCache::install(*parsed.dataset, path, cacheRoot, callbacks);
    QVERIFY2(installed.ok, qPrintable(installed.error));
    QVERIFY(QFile::exists(OsmxCache::dataPath(cacheRoot, QStringLiteral("cache-dataset"))));
    QVERIFY(QFile::exists(OsmxCache::manifestPath(cacheRoot, QStringLiteral("cache-dataset"))));
    QVERIFY(!progress.isEmpty());
    QCOMPARE(progress.last(), 100);
    for (int i = 1; i < progress.size(); ++i) QVERIFY(progress.at(i) >= progress.at(i - 1));

    const OsmxCacheListing listing = OsmxCache::list(cacheRoot);
    QVERIFY2(listing.problems.isEmpty(), qPrintable(listing.problems.join(QStringLiteral("; "))));
    QCOMPARE(listing.entries.size(), qsizetype(1));
    const auto entry = listing.entries.first();
    QVERIFY(entry.ok);
    QCOMPARE(entry.sourceFormat, QStringLiteral("osmx"));
    QCOMPARE(entry.datasetId, QStringLiteral("cache-dataset"));
    QCOMPARE(entry.stats.nodes, quint64(5));
    QCOMPARE(entry.stats.ways, quint64(5));
    QCOMPARE(entry.stats.relations, quint64(2));
    QCOMPARE(entry.hasBounds, true);
    QCOMPARE(entry.cachePath, OsmxCache::datasetDirectory(cacheRoot, QStringLiteral("cache-dataset")));
    QCOMPARE(entry.sourceSha256, parsed.sha256);

    // The cached copy re-parses to the same identity and counts.
    const OsmxParseResult reopened =
        parseFile(OsmxCache::dataPath(cacheRoot, QStringLiteral("cache-dataset")), {}, QStringLiteral("cache-dataset"));
    QVERIFY2(reopened.ok(), qPrintable(reopened.error.toString()));
    QCOMPARE(reopened.sha256, parsed.sha256);
    QCOMPARE(reopened.dataset->info().directedEdgeCount, parsed.dataset->info().directedEdgeCount);

    QString removeError;
    QVERIFY(OsmxCache::remove(cacheRoot, QStringLiteral("cache-dataset"), &removeError));
    QVERIFY(!QDir(OsmxCache::datasetDirectory(cacheRoot, QStringLiteral("cache-dataset"))).exists());
    QVERIFY(!OsmxCache::isSafeDatasetId(QStringLiteral("../escape")));
}

void OsmxTests::skipsCorruptCacheEntries() {
    // Test sandboxes (and some Windows service contexts) deny writes to the
    // system temp directory, so keep fixtures next to the test binary.
    QTemporaryDir directory(QDir::currentPath() + QStringLiteral("/p1-osmx-test-XXXXXX"));
    QVERIFY(directory.isValid());
    const QString cacheRoot = directory.filePath(QStringLiteral("cache"));
    QVERIFY(QDir().mkpath(QDir(cacheRoot).absoluteFilePath(QStringLiteral("broken"))));
    QFile manifest(QDir(cacheRoot).absoluteFilePath(QStringLiteral("broken/manifest.json")));
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write("{not json");
    manifest.close();

    const OsmxCacheListing listing = OsmxCache::list(cacheRoot);
    QCOMPARE(listing.entries.size(), qsizetype(0));
    QCOMPARE(listing.problems.size(), qsizetype(1));
    QVERIFY(listing.problems.first().contains(QStringLiteral("broken")));
}

void OsmxTests::storePublishesSharedDataset() {
    // Test sandboxes (and some Windows service contexts) deny writes to the
    // system temp directory, so keep fixtures next to the test binary.
    QTemporaryDir directory(QDir::currentPath() + QStringLiteral("/p1-osmx-test-XXXXXX"));
    QVERIFY(directory.isValid());
    const QString path = writeFixture(buildFixture(syntheticFixture()), directory, QStringLiteral("store.osmx"));
    const OsmxParseResult parsed = parseFile(path, {}, QStringLiteral("store-dataset"));
    QVERIFY(parsed.ok());

    OsmxStore& store = OsmxStore::shared();
    store.clear();
    QVERIFY(store.active() == nullptr);
    store.publish(parsed.dataset);
    QCOMPARE(store.activeId(), QStringLiteral("store-dataset"));
    QCOMPARE(store.active().get(), parsed.dataset.get());
    QCOMPARE(store.find(QStringLiteral("store-dataset")).get(), parsed.dataset.get());
    QVERIFY(store.find(QStringLiteral("missing")) == nullptr);
    QCOMPARE(store.datasetIds(), QVector<QString>({QStringLiteral("store-dataset")}));
    store.clear();
    QVERIFY(store.active() == nullptr);
}

void OsmxTests::parsesRealFileWhenProvided() {
    const QByteArray envPath = qgetenv("P1_OSMX_REAL_FILE");
    if (envPath.isEmpty()) QSKIP("P1_OSMX_REAL_FILE is not set; skipping the 47 MiB real-file check");
    const QString path = QString::fromLocal8Bit(envPath);
    const OsmxParseResult result = parseFile(path, {}, QStringLiteral("wuhan-real"));
    QVERIFY2(result.ok(), qPrintable(result.error.toString()));
    const auto& dataset = *result.dataset;
    QCOMPARE(dataset.nodeCount(), 912730u);
    QCOMPARE(dataset.wayCount(), 121964u);
    QCOMPARE(dataset.relationCount(), 1658u);
    QCOMPARE(result.sha256, QStringLiteral("a9c4d90b049a5e34ed37c3dfecb7d76a6ff96425c1da7032536199fb2ef16467"));
    QCOMPARE(dataset.info().routableWayCount, 44986u);
    QCOMPARE(dataset.info().directedEdgeCount, 536688u);
    QCOMPARE(dataset.bounds().minLat, 29.833451);
    QCOMPARE(dataset.bounds().maxLon, 114.8508807);
    const auto nearest = dataset.nearestNode(30.52, 114.36, 500.0);
    QVERIFY(nearest.has_value());
    QVERIFY(dataset.nodeIndex(dataset.nodeAt(*nearest).id).has_value());
}

QTEST_GUILESS_MAIN(OsmxTests)
#include "OsmxTests.moc"
