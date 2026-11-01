#include "qt/FmmMapMatcher.h"
#include <QFile>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QtTest>
#include <cmath>
#include <limits>

using namespace p1::algorithms;
namespace c = p1::contracts;
namespace {
bool touch(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly);
}
FmmConfig config(const QString& network, const QString& ubodt, const QString& executable) {
    FmmConfig cfg;
    cfg.executable = executable; cfg.networkFile = network; cfg.ubodtFile = ubodt;
    cfg.datasetId = QStringLiteral("official-synthetic-example");
    cfg.networkVersion = cfg.ubodtNetworkVersion = QStringLiteral("19ef34e1-example");
    cfg.coordinateDefinition = QStringLiteral("Upstream artificial planar coordinates; NOT real WGS84 metres");
    cfg.units = FmmUnits::SyntheticSampleUnits;
    cfg.candidates = 4; cfg.radius = 0.4; cfg.gpsError = 0.5;
    return cfg;
}
// ONLY official artificial fixture coordinates are labelled as degrees to test
// the public interface. This identity is NOT a production projection.
FmmCoordinateTransform syntheticTransform() {
    return {[](const c::GeoCoordinate& p) -> std::optional<FmmPoint> {
                return FmmPoint{p.longitudeDeg, p.latitudeDeg};
            }, [](FmmPoint p) -> std::optional<c::GeoCoordinate> {
                return c::GeoCoordinate{c::CoordinateSystem::WGS84, p.y, p.x, {}};
            }};
}
c::MapMatchRequest request(std::uint64_t id, FmmPoint p = {1, 1}) {
    c::MapMatchRequest req;
    req.requestId = id; req.locationSeq = id;
    req.position = {c::CoordinateSystem::WGS84, p.y, p.x, {}};
    return req;
}
struct Fixture {
    QTemporaryDir dir;
    QString network = dir.filePath(QStringLiteral("network.shp"));
    QString ubodt = dir.filePath(QStringLiteral("ubodt.txt"));
    Fixture() { touch(network); touch(ubodt); }
    ~Fixture() {
        touch(network + QStringLiteral(".release"));
        QThreadPool::globalInstance()->waitForDone(5000);
    }
    FmmConfig cfg() const {
        return config(network, ubodt, qEnvironmentVariable("P1_FMM_PROCESS_FIXTURE"));
    }
    bool started() const { return QFile::exists(network + QStringLiteral(".started")); }
    void release() { touch(network + QStringLiteral(".release")); }
    void closeGate() {
        QFile::remove(network + QStringLiteral(".release"));
        QFile::remove(network + QStringLiteral(".started"));
    }
};
} // namespace

class FmmTests : public QObject {
    Q_OBJECT
private slots:
    void parseResults();
    void invalidBatch();
    void windowAndInvalidRequests();
    void asyncLifecycle();
    void failureDelivery();
    void officialSample_data();
    void officialSample();
};

void FmmTests::parseResults() {
    const QByteArray good = "id;opath;error;pgeom;cpath;mgeom\n1;-7,5;0.1,0.2;LINESTRING(1 2,3 4);-7,5;LINESTRING(1 2,3 4)\n";
    auto parsed = parseFmmOutput(good, 2);
    QCOMPARE(parsed.status, FmmStatus::Matched);
    QCOMPARE(parsed.observationEdges.back().value, std::int64_t(5));
    QCOMPARE(parsed.completePath.front().value, std::int64_t(-7));
    QCOMPARE(parsed.projectedPoints.back().y, 4.0);
    QCOMPARE(parseFmmOutput(good, 3).status, FmmStatus::InvalidOutput);
    // Exact pinned writer shape for match_traj's no-candidate MatchResult{}.
    const QByteArray empty = "id;opath;error;pgeom;cpath;mgeom\n0;;;;;LINESTRING()\n";
    QCOMPARE(parseFmmOutput(empty, 2).status, FmmStatus::NoMatch);
    auto noPath = good;
    noPath.replace(";-7,5;LINESTRING(1 2,3 4)\n", ";;LINESTRING()\n");
    QCOMPARE(parseFmmOutput(noPath, 2).status, FmmStatus::NoMatch);
    noPath.replace("0.1,0.2", "nan,0.2");
    QCOMPARE(parseFmmOutput(noPath, 2).status, FmmStatus::InvalidOutput);
    QCOMPARE(parseFmmOutput("id;opath;error;pgeom;cpath;mgeom\n1;;;;;LINESTRING()\n", 2).status, FmmStatus::NoMatch);
    // Empty cpath alone never makes damaged data a valid NoMatch.
    for (const auto& malformed : {
             QByteArray("id;opath;error;pgeom;cpath;mgeom\n0;;;;;\n"),
             QByteArray("id;opath;error;pgeom;cpath;mgeom\n0;;;;;LINESTRING(bad)\n"),
             QByteArray("id;opath;error;pgeom;cpath;mgeom\n0;-7;;;;LINESTRING()\n"),
             QByteArray("id;opath;error;pgeom;cpath;mgeom\n0;;nan;;;LINESTRING()\n"),
             QByteArray("id;opath;error;pgeom;cpath;mgeom\n2;;;;;LINESTRING()\n"),
             QByteArray("id;opath;error;pgeom;cpath;mgeom\n1;;;;;\n"),
             QByteArray("id;opath;error;pgeom;cpath\n0;;;;\n"),
             QByteArray("id;opath;error;pgeom;cpath;cpath\n0;;;;;LINESTRING()\n")})
        QCOMPARE(parseFmmOutput(malformed, 2).status, FmmStatus::InvalidOutput);
    auto wrongId = good;
    wrongId.replace("\n1;", "\n0;");
    QCOMPARE(parseFmmOutput(wrongId, 2).status, FmmStatus::InvalidOutput);
    wrongId = good; wrongId.replace("\n1;", "\n2;");
    QCOMPARE(parseFmmOutput(wrongId, 2).status, FmmStatus::InvalidOutput);
    QCOMPARE(parseFmmOutput({}, 2).status, FmmStatus::InvalidOutput);
    auto invalid = good;
    invalid.replace("0.1,0.2", "nan,0.2");
    QCOMPARE(parseFmmOutput(invalid, 2).status, FmmStatus::InvalidOutput);
    invalid = good; invalid.replace("-7,5;LINESTRING", "bad;LINESTRING");
    QCOMPARE(parseFmmOutput(invalid, 2).status, FmmStatus::InvalidOutput);
    invalid = good; invalid.replace("LINESTRING(1 2,3 4)", "LINESTRING(1 2 3,3 4)");
    QCOMPARE(parseFmmOutput(invalid, 2).status, FmmStatus::InvalidOutput);
}
void FmmTests::invalidBatch() {
    Fixture fixture;
    auto cfg = fixture.cfg();
    QCOMPARE(runFmm(cfg, {}).status, FmmStatus::InvalidInput);
    QCOMPARE(runFmm(cfg, {{1, 1}}).status, FmmStatus::InvalidInput);
    QCOMPARE(runFmm(cfg, {{1, 1}, {std::numeric_limits<double>::infinity(), 2}}).status, FmmStatus::InvalidInput);
    cfg.ubodtNetworkVersion = QStringLiteral("other-version");
    QCOMPARE(runFmm(cfg, {{1, 1}, {2, 2}}).status, FmmStatus::InvalidConfiguration);
    cfg = fixture.cfg(); cfg.networkFile += QStringLiteral(".missing");
    QCOMPARE(runFmm(cfg, {{1, 1}, {2, 2}}).status, FmmStatus::InvalidConfiguration);
    auto flag = std::make_shared<std::atomic_bool>(true);
    QCOMPARE(runFmm(fixture.cfg(), {{1, 1}, {2, 2}}, flag).status, FmmStatus::Cancelled);
    cfg = fixture.cfg(); cfg.timeoutMs = 1000;
    QCOMPARE(runFmm(cfg, {{1, 1}, {2, 2}}).status, FmmStatus::TimedOut);
    fixture.release();
    cfg = fixture.cfg(); cfg.p0EdgeIds.emplace(100, model::EdgeId{999});
    QCOMPARE(runFmm(cfg, {{1, 1}, {2, 2}}).status, FmmStatus::InvalidOutput);
    cfg = fixture.cfg(); cfg.p0EdgeIds.emplace(-7, model::EdgeId{900});
    const auto mapped = runFmm(cfg, {{1, 1}, {2, 2}});
    QCOMPARE(mapped.status, FmmStatus::Matched);
    QCOMPARE(mapped.p0ObservationEdges.back().value, std::uint64_t(900));
    QFile mode(fixture.ubodt);
    QVERIFY(mode.open(QIODevice::WriteOnly));
    mode.write("exit-without-output"); mode.close();
    QCOMPARE(runFmm(fixture.cfg(), {{1, 1}, {2, 2}}).status, FmmStatus::InvalidOutput);
    QVERIFY(mode.open(QIODevice::WriteOnly | QIODevice::Truncate));
    mode.write("fail"); mode.close();
    QCOMPARE(runFmm(fixture.cfg(), {{1, 1}, {2, 2}}).status, FmmStatus::ExecutionFailed);
}
void FmmTests::windowAndInvalidRequests() {
    Fixture fixture;
    auto cfg = fixture.cfg();
    cfg.p0EdgeIds.emplace(-7, model::EdgeId{7});
    auto& writableAlias = cfg.p0EdgeIds.at(-7);
    FmmMapMatcher matcher(cfg, syntheticTransform(), 2);
    writableAlias = model::EdgeId{999}; // published config must have its own copy
    cfg.ubodtNetworkVersion = QStringLiteral("changed-after-construction");
    QSignalSpy ready(&matcher, &c::MapMatcher::matchReady);
    QCOMPARE(matcher.requestMatch(request(0)), std::uint64_t(0)); // requestId still positive
    auto first = request(1); first.locationSeq = 0;
    QCOMPARE(matcher.requestMatch(first), std::uint64_t(1));
    QTRY_COMPARE(ready.size(), 1);
    QCOMPARE(matcher.lastDetails()->batch.status, FmmStatus::WaitingForPoints);
    QCOMPARE(qvariant_cast<c::MapMatchResult>(ready.back()[0]).locationSeq, std::uint64_t(0));
    QVERIFY(!qvariant_cast<c::MapMatchResult>(ready.back()[0]).matched);
    QCOMPARE(matcher.requestMatch(request(1)), std::uint64_t(0));
    auto bad = request(2); bad.position.latitudeDeg = 91;
    QCOMPARE(matcher.requestMatch(bad), std::uint64_t(0));
    bad = request(2); bad.position.longitudeDeg = std::numeric_limits<double>::quiet_NaN();
    QCOMPARE(matcher.requestMatch(bad), std::uint64_t(0));
    bad = request(2); bad.locationSeq = 0;
    QCOMPARE(matcher.requestMatch(bad), std::uint64_t(0));
    bad = request(2); bad.position.crs = c::CoordinateSystem::GCJ02;
    QCOMPARE(matcher.requestMatch(bad), std::uint64_t(0));
    fixture.release();
    auto second = request(2); second.locationSeq = 1;
    QCOMPARE(matcher.requestMatch(second), std::uint64_t(2)); // equal geometry preserved
    QTRY_COMPARE(ready.size(), 2);
    QCOMPARE(matcher.lastDetails()->batch.inputPointCount, std::size_t(2));
    QCOMPARE(matcher.lastDetails()->batch.status, FmmStatus::Matched);
    QCOMPARE(matcher.lastDetails()->batch.p0ObservationEdges.back().value, std::uint64_t(7));
    bad = request(3); bad.locationSeq = 0; // decreasing
    QCOMPARE(matcher.requestMatch(bad), std::uint64_t(0));
    bad.locationSeq = 1; // duplicate
    QCOMPARE(matcher.requestMatch(bad), std::uint64_t(0));
    QCOMPARE(matcher.requestMatch(request(3, {2, 2})), std::uint64_t(3));
    QTRY_COMPARE(ready.size(), 3);
    QCOMPARE(matcher.lastDetails()->batch.inputPointCount, std::size_t(2)); // bounded window
    const auto retained = matcher.lastDetails();
    auto changedRoute = request(4); changedRoute.routeId = QStringLiteral("other-route");
    QCOMPARE(matcher.requestMatch(changedRoute), std::uint64_t(4));
    QTRY_COMPARE(ready.size(), 4);
    QCOMPARE(matcher.lastDetails()->batch.status, FmmStatus::WaitingForPoints);
    QCOMPARE(retained->batch.status, FmmStatus::Matched); // owning immutable result survives replacement
    matcher.resetWindow();
    ready.clear();
    auto newSession = request(5); newSession.locationSeq = 0;
    QCOMPARE(matcher.requestMatch(newSession), std::uint64_t(5));
    QTRY_COMPARE(ready.size(), 1);
    QCOMPARE(matcher.lastDetails()->batch.status, FmmStatus::WaitingForPoints);
    QCOMPARE(qvariant_cast<c::MapMatchResult>(ready.back()[0]).locationSeq, std::uint64_t(0));
    bad = request(6); bad.locationSeq = 0;
    QCOMPARE(matcher.requestMatch(bad), std::uint64_t(0));
    bad = request(5); bad.locationSeq = 1; // requestId still strictly increasing after reset
    QCOMPARE(matcher.requestMatch(bad), std::uint64_t(0));
    auto next = request(6); next.locationSeq = 1;
    QCOMPARE(matcher.requestMatch(next), std::uint64_t(6));
    QTRY_COMPARE(ready.size(), 2);
    QCOMPARE(matcher.lastDetails()->batch.status, FmmStatus::Matched);
    matcher.resetWindow();
    ready.clear();
    auto queued = request(7); queued.locationSeq = 0;
    QCOMPARE(matcher.requestMatch(queued), std::uint64_t(7));
    matcher.resetWindow(); // suppress queued waiting response
    QTest::qWait(20);
    QCOMPARE(ready.size(), 0);
}
void FmmTests::asyncLifecycle() {
    Fixture fixture;
    auto* matcher = new FmmMapMatcher(fixture.cfg(), syntheticTransform());
    std::unique_ptr<FmmMapMatcher> owner(matcher);
    QSignalSpy ready(matcher, &c::MapMatcher::matchReady);
    c::MapMatcher* publicInterface = matcher;
    QCOMPARE(publicInterface->requestMatch(request(1)), std::uint64_t(1));
    QTRY_COMPARE(ready.size(), 1);
    ready.clear();
    int beats = 0;
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, this, [&] { ++beats; });
    heartbeat.start(1);
    QCOMPARE(publicInterface->requestMatch(request(2)), std::uint64_t(2));
    QTRY_VERIFY(fixture.started());
    QTRY_VERIFY(beats > 0); // worker child is gated, GUI event loop continues
    QCOMPARE(ready.size(), 0);
    QCOMPARE(publicInterface->requestMatch(request(3, {2, 2})), std::uint64_t(3));
    fixture.release();
    QTRY_COMPARE(ready.size(), 1);
    const auto output = qvariant_cast<c::MapMatchResult>(ready.back()[0]);
    QCOMPARE(output.requestId, std::uint64_t(3));
    QCOMPARE(output.locationSeq, std::uint64_t(3));
    QVERIFY(output.matched);
    QCOMPARE(output.matched->longitudeDeg, 2.0);
    QVERIFY(!output.distanceToRouteM && !output.alongRouteDistanceM);
    QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
    QCoreApplication::processEvents();
    QCOMPARE(ready.size(), 1); // old result cannot overwrite new one
    fixture.closeGate();
    QCOMPARE(publicInterface->requestMatch(request(4)), std::uint64_t(4));
    QTRY_VERIFY(fixture.started());
    matcher->cancel(4);
    fixture.release();
    QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
    QCoreApplication::processEvents();
    QCOMPARE(ready.size(), 1);
    fixture.closeGate();
    QCOMPARE(publicInterface->requestMatch(request(5)), std::uint64_t(5));
    QTRY_VERIFY(fixture.started());
    owner.reset(); // no worker uses `this`; watcher disconnects
    fixture.release();
    QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
}
void FmmTests::failureDelivery() {
    Fixture fixture;
    auto cfg = fixture.cfg(); cfg.executable = fixture.dir.filePath(QStringLiteral("missing-fmm"));
    FmmMapMatcher matcher(cfg, syntheticTransform());
    QSignalSpy ready(&matcher, &c::MapMatcher::matchReady);
    matcher.requestMatch(request(1));
    QTRY_COMPARE(ready.size(), 1);
    matcher.requestMatch(request(2));
    QTRY_COMPARE(ready.size(), 2);
    QCOMPARE(matcher.lastDetails()->batch.status, FmmStatus::InvalidConfiguration);
    QVERIFY(!qvariant_cast<c::MapMatchResult>(ready.back()[0]).matched);
}
void FmmTests::officialSample_data() {
    QTest::addColumn<int>("track");
    QTest::addColumn<QString>("expected");
    QTest::newRow("track-1") << 1 << QStringLiteral("2,5,13,14,23");
    QTest::newRow("track-2") << 2 << QStringLiteral("25,4,3,5,17,19");
    QTest::newRow("track-3") << 3 << QStringLiteral("8,11,13,18,20,24");
}
void FmmTests::officialSample() {
    QFETCH(int, track);
    QFETCH(QString, expected);
    const auto data = qEnvironmentVariable("P1_FMM_SAMPLE_DIR");
    auto cfg = config(data + QStringLiteral("/edges.shp"), qEnvironmentVariable("P1_FMM_SAMPLE_UBODT"),
                      qEnvironmentVariable("P1_FMM_EXECUTABLE"));
    QFile trips(data + QStringLiteral("/trips.csv"));
    QVERIFY2(trips.open(QIODevice::ReadOnly), "Supply the external pinned official fixture directory");
    const auto lines = trips.readAll().split('\n');
    QVERIFY(track < lines.size());
    std::vector<FmmPoint> points;
    QVERIFY(parseFmmLineString(QString::fromUtf8(lines[track].mid(lines[track].indexOf(';') + 1)), points));
    const auto result = runFmm(cfg, points); // true fmm/HMM, fresh temp output
    QVERIFY2(result.status == FmmStatus::Matched, qPrintable(result.message + result.processLog));
    QStringList roads;
    for (const auto id : result.completePath) roads.push_back(QString::number(id.value));
    QCOMPARE(roads.join(','), expected);
    QVERIFY(!result.matchedGeometry.empty());
    QCOMPARE(result.projectedPoints.size(), points.size());
    QVERIFY(std::abs(result.matchedGeometry.back().x - result.projectedPoints.back().x) < 1e-8);
    QVERIFY(std::abs(result.matchedGeometry.back().y - result.projectedPoints.back().y) < 1e-8);
    qInfo().noquote() << "Real FMM HMM track" << track << "roads=" << roads.join(',')
                      << "points=" << points.size() << "call_elapsed_ms=" << result.elapsedMs;
    const auto artifactDir = qEnvironmentVariable("P1_FMM_RESULT_DIR");
    if (!artifactDir.isEmpty()) {
        QVERIFY(QDir().mkpath(artifactDir));
        QFile artifact(artifactDir + QStringLiteral("/track-%1.csv").arg(track));
        QVERIFY(artifact.open(QIODevice::WriteOnly));
        QCOMPARE(artifact.write(result.outputCsv), qint64(result.outputCsv.size()));
    }
    // Same official trajectory through the single-point public Qt contract.
    FmmMapMatcher matcher(cfg, syntheticTransform());
    QSignalSpy ready(&matcher, &c::MapMatcher::matchReady);
    for (std::size_t i = 0; i < points.size(); ++i)
        QCOMPARE(matcher.requestMatch(request(i + 1, points[i])), std::uint64_t(i + 1));
    QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 30000);
    QVERIFY2(matcher.lastDetails()->batch.status == FmmStatus::Matched,
             qPrintable(matcher.lastDetails()->batch.message + matcher.lastDetails()->batch.processLog));
    const auto output = qvariant_cast<c::MapMatchResult>(ready.back()[0]);
    QVERIFY(output.matched);
    QCOMPARE(output.requestId, std::uint64_t(points.size()));
    QVERIFY(std::abs(output.matched->longitudeDeg - result.projectedPoints.back().x) < 1e-8);
    QVERIFY(std::abs(output.matched->latitudeDeg - result.projectedPoints.back().y) < 1e-8);
    const auto noMatch = runFmm(cfg, {{80, 80}, {81, 81}});
    QCOMPARE(noMatch.status, FmmStatus::NoMatch); // real out-of-network input
}
QTEST_GUILESS_MAIN(FmmTests)
#include "FmmTests.moc"
