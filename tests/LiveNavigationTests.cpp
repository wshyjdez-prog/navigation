#include "app/NavigationWorkflow.h"
#include "app/NavigationRuntime.h"
#include "algorithms/qt/FmmMapMatcher.h"
#include "data/import/OsmImportParser.h"
#include "navigation/MockLocationSource.h"
#include <QElapsedTimer>
#include <QDateTime>
#include <QSemaphore>
#include <QThread>

#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <cmath>
#include <limits>

using namespace p1;
using algorithms::FmmStatus;

namespace {
constexpr double routeLat = 30.593;
constexpr double parallelLat = 30.597;
constexpr double isolatedLat = 30.601;
constexpr qint64 epoch = 1767225600000;

bool writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

template<class Predicate>
bool await(Predicate predicate, int timeout = 6000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) QTest::qWait(10);
    return predicate();
}

QByteArray osmFixture() {
    QByteArray xml("<osm version='0.6'>");
    auto addRow = [&](int first, double latitude) {
        for (int i = 0; i < 7; ++i)
            xml += QString("<node id='%1' lat='%2' lon='%3'/>")
                .arg(first + i).arg(latitude,0,'f',7).arg(114.300 + i*0.001,0,'f',7).toUtf8();
    };
    auto addWay = [&](int id, std::initializer_list<int> refs, const char* highway, bool oneWay = false) {
        xml += "<way id='" + QByteArray::number(id) + "'>";
        for (const int ref : refs) xml += "<nd ref='" + QByteArray::number(ref) + "'/>";
        xml += "<tag k='highway' v='" + QByteArray(highway) + "'/>";
        if (oneWay) xml += "<tag k='oneway' v='yes'/>";
        xml += "</way>";
    };
    addRow(1, routeLat);
    addRow(11, parallelLat);
    addRow(21, isolatedLat);
    addRow(31, routeLat - 0.0001);
    addWay(100, {1,2,3,4,5,6,7}, "residential");
    addWay(101, {11,12,13,14,15,16,17}, "residential", true);
    addWay(102, {21,22,23,24,25,26,27}, "residential");
    addWay(103, {4,14}, "residential");
    addWay(104, {7,17}, "residential");
    addWay(105, {31,32,33,34,35,36,37}, "footway");
    return xml + "</osm>";
}


constexpr double corridorLon(double metres) { return 114.300 + metres * 0.00001044; }

QByteArray orderedCorridor(int step, int finish, bool bidirectional = false) {
    QByteArray xml("<osm version='0.6'>");
    for (auto [first, lat] : {std::pair{1, routeLat}, std::pair{101, parallelLat}}) {
        for (int x = 0; x <= finish; x += step)
            xml += QString("<node id='%1' lat='%2' lon='%3'/>").arg(first + x / step)
                .arg(lat, 0, 'f', 7).arg(corridorLon(x), 0, 'f', 9).toUtf8();
    }
    auto way = [&](int id, const QByteArray& refs, bool oneway = true) {
        xml += "<way id='" + QByteArray::number(id) + "'>" + refs +
               "<tag k='highway' v='residential'/>";
        if (oneway) xml += "<tag k='oneway' v='yes'/>";
        xml += "</way>";
    };
    const auto ref = [](int id) { return "<nd ref='" + QByteArray::number(id) + "'/>"; };
    for (int first : {1, 101}) {
        QByteArray refs;
        for (int i = 0; i <= finish / step; ++i) refs += ref(first + i);
        way(first + 1000, refs, first == 1 || !bidirectional);
    }
    way(2000, ref(101 + finish / step) + ref(1 + finish / step));
    return xml + "</osm>";
}


contracts::PositionSample sample(quint64 seq, int second, double lat, double lon, quint64 session = 1) {
    contracts::PositionSample fix;
    fix.coordinate = {contracts::CoordinateSystem::WGS84, lat, lon, {}};
    fix.seq = seq;
    fix.timestampMs = epoch + qint64(second)*1000;
    fix.sessionId = session;
    fix.dataVersion = "phase3-synthetic";
    return fix;
}

QByteArray shellQuote(const QString& path) {
    QByteArray result = path.toUtf8();
    result.replace("'", "'\\''");
    return "'" + result + "'";
}

struct Scenario {
    QTemporaryDir dir;
    app::NavigationWorkflow flow;
    app::LiveNavigationOptions options;
    quint64 seq{};

    Scenario() {
        options.fmmExecutable = qEnvironmentVariable("P1_FMM_EXECUTABLE");
        options.ubodtExecutable = qEnvironmentVariable("P1_UBODT_EXECUTABLE");
        options.fixGapMs = 5000;
    }

    bool load(const QByteArray& fixture = osmFixture(), double destinationLon = 114.306) {
        const auto path = dir.filePath("roads.osm");
        if (!writeFile(path, fixture)) return false;
        const auto entry = data::OsmImportParser(dir.filePath("cache")).parse(path, {}, {}, {});
        if (!entry.ok) return false;
        QSignalSpy done(&flow, &app::NavigationWorkflow::completed);
        flow.load(entry, 30.596, 114.303, 1.5);
        if (!await([&]{ return done.count() == 1; }) || !done.last()[1].toBool()) return false;
        flow.setRouteMode(app::RouteMode::Driving);
        flow.plan({114.300, routeLat}, {destinationLon, routeLat});
        return await([&]{ return done.count() == 2; }) && done.last()[1].toBool();
    }

    bool start(contracts::LocationSource* input = nullptr) {
        if (!flow.startNavigation(input, options)) return false;
        return await([&] { return flow.liveNavigation().matchStatus == FmmStatus::WaitingForPoints &&
            !flow.liveNavigation().preparing; });
    }

    void push(int second, double lat, double lon, quint64 session = 1) {
        flow.locationUpdated(sample(seq++, second, lat, lon, session));
    }

    bool match(int second, double lat, double lon) {
        const auto sent = seq;
        push(second, lat, lon);
        return await([&] {
            const auto& value = flow.liveNavigation();
            return value.match && value.match->locationSeq == sent;
        });
    }

    bool waiting() {
        return await([&] { return flow.navigationTaskCount() == 0 &&
            flow.liveNavigation().matchStatus == FmmStatus::WaitingForPoints; });
    }
};


QList<QByteArray> readLines(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed().split('\n') : QList<QByteArray>{};
}

bool recordNetworks(Scenario& s, const QString& marker) {
    const auto wrapper = s.dir.filePath("fmm-record-path");
    if (!writeFile(wrapper, "#!/bin/sh\nprev=\nfor arg do\n"
        "  if [ \"$prev\" = --network ]; then printf '%s\\n' \"$arg\" >> " + shellQuote(marker) +
        "; break; fi\n  prev=$arg\ndone\nexec " + shellQuote(s.options.fmmExecutable) + " \"$@\"\n") ||
        !QFile::setPermissions(wrapper,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner)) return false;
    s.options.fmmExecutable = wrapper;
    return true;
}


void showMeasurements(const app::NavigationWorkflow& flow, int ticks) {
    const auto& live = flow.liveNavigation();
    qInfo().noquote() << "phase3 fixture"
                      << "export_ms" << live.exportMs << "ubodt_generate_ms" << live.ubodtMs
                      << "ubodt_load_ms" << (live.ubodtLoadMs ? QString::number(*live.ubodtLoadMs) : QString("unavailable"))
                      << "fmm_window_ms" << live.matchMs << "astar_ms" << live.planMs
                      << "cancel_ms" << live.cancellationMs << "ui_heartbeat_ticks" << ticks
                      << flow.runtimeStatus();
}
}

class LiveNavigationTests : public QObject {
    Q_OBJECT
private slots:

    void gapAndStaleRegression() {
        Scenario s;
        QVERIFY(s.load());
        const auto marker = s.dir.filePath("fmm-starts");
        const auto wrapper = s.dir.filePath("fmm-gate");
        QVERIFY(writeFile(wrapper, "#!/bin/sh\nprintf 'start\\n' >> " + shellQuote(marker) +
            "\nsleep .35\nexec " + shellQuote(s.options.fmmExecutable) + " \"$@\"\n"));
        QVERIFY(QFile::setPermissions(wrapper,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
        s.options.fmmExecutable = wrapper;
        QVERIFY(s.start());
        QSignalSpy requests(&s.flow, &app::NavigationWorkflow::navigationRerouteRequested);
        QList<quint64> published;
        connect(&s.flow, &app::NavigationWorkflow::navigationMatchUpdated, &s.flow, [&] {
            if (s.flow.liveNavigation().match) published.append(s.flow.liveNavigation().match->locationSeq);
        });
        s.seq = 9;
        s.push(0, parallelLat, 114.3005);
        QVERIFY(s.waiting());
        QVERIFY(s.match(1, parallelLat, 114.301));
        auto starts = [&] {
            QFile f(marker);
            return f.open(QIODevice::ReadOnly) ? f.readAll().count('\n') : 0;
        };
        s.push(2, parallelLat, 114.3013); // 11
        QVERIFY(await([&] { return starts() == 2; }));
        s.push(3, routeLat, 114.3016); // 12 is overwritten
        s.push(4, parallelLat, 114.3019); // 13
        QVERIFY(await([&] { return starts() == 3; }));
        s.push(5, routeLat, 114.3022); // 14 is unconfirmed while 13 returns
        QVERIFY(await([&] { return s.flow.navigationTaskCount() == 0; }));
        qInfo() << "published" << published << "requests" << requests.count();
        if (!requests.isEmpty()) {
            const auto request = qvariant_cast<contracts::RerouteRequest>(requests[0][0]);
            qInfo() << "unconfirmed reroute source" << request.locationSeq.value_or(0)
                    << request.currentPosition.latitudeDeg;
        }
        QVERIFY(!published.contains(11) && !published.contains(12) && !published.contains(13));
        QCOMPARE(requests.count(),0);
    }

    void missingDbfRegression() {
        Scenario s;
        QVERIFY(s.load());
        const auto marker = s.dir.filePath("used-networks");
        const auto wrapper = s.dir.filePath("fmm-paths");
        QVERIFY(writeFile(wrapper, "#!/bin/sh\nprev=\nfor arg do\n"
            "  if [ \"$prev\" = --network ]; then printf '%s\\n' \"$arg\" >> " + shellQuote(marker) +
            "; break; fi\n  prev=$arg\ndone\nexec " + shellQuote(s.options.fmmExecutable) + " \"$@\"\n"));
        QVERIFY(QFile::setPermissions(wrapper,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
        s.options.fmmExecutable = wrapper;
        QVERIFY(s.start());
        s.push(1, routeLat, 114.3005);
        QVERIFY(s.waiting());
        QVERIFY(s.match(2, routeLat, 114.301));
        auto starts = [&] {
            QFile f(marker);
            return f.open(QIODevice::ReadOnly) ? f.readAll().trimmed().split('\n') : QList<QByteArray>{};
        };
        const auto original = starts().value(0);
        QVERIFY(original.endsWith(".shp"));
        QVERIFY(QFile::remove(QString::fromUtf8(original.chopped(4) + ".dbf")));
        s.push(3, routeLat, 114.3015);
        QVERIFY(await([&]{return s.flow.navigationTaskCount() == 0;}));
        QTest::qWait(1100);
        s.push(4, routeLat, 114.302);
        QVERIFY(await([&]{return s.flow.navigationTaskCount() == 0;}));
        const bool recovered = s.match(5, routeLat, 114.3025);
        qInfo() << "FMM networks" << starts() << "preparations" << s.flow.liveNavigation().preparationCount;
        QVERIFY(recovered);
        QCOMPARE(starts().count(original), 1);
        QCOMPARE(s.flow.liveNavigation().preparationCount, std::size_t(2));
    }


    void movingCandidate_data() {
        QTest::addColumn<int>("step");
        QTest::newRow("100m-edge-stopped-at-40m") << 100;
        QTest::newRow("10m-edges-moving-10m-per-second") << 10;
    }

    void movingCandidate() {
        QFETCH(int, step);
        Scenario s;
        constexpr int finish = 200;
        QVERIFY2(s.load(orderedCorridor(step, finish), corridorLon(finish)), qPrintable(s.flow.status()));
        s.options.windowSize = 3;
        s.options.rerouteResultReady = [] { QThread::msleep(1300); };
        QVERIFY(s.start());
        QSignalSpy requests(&s.flow, &app::NavigationWorkflow::navigationRerouteRequested);
        QSignalSpy results(&s.flow, &app::NavigationWorkflow::navigationRerouteFinished);
        const auto originalDestination = s.flow.state().route.last();
        QElapsedTimer clock;
        clock.start();
        s.push(-1, parallelLat, corridorLon(step == 100 ? 40 : 1));
        QVERIFY(s.waiting());
        const auto at = [&](int second) { return step == 100 ? 40 : 2 + 10 * second; };
        for (int second = 0; second <= 4; ++second) {
            QVERIFY(await([&] { return clock.elapsed() >= 1000 * second; }));
            const auto seq = s.seq;
            s.push(second, parallelLat, corridorLon(at(second)));
            QVERIFY2(await([&] {
                const auto& current = s.flow.liveNavigation().match;
                return (current && current->locationSeq == seq) || (second == 4 && !results.isEmpty());
            }), qPrintable(s.flow.status()));
            if (second == 3) {
                QCOMPARE(requests.count(), 1);
                QVERIFY(s.flow.liveNavigation().rerouting);
                QVERIFY(results.isEmpty());
                const auto request = qvariant_cast<contracts::RerouteRequest>(requests[0][0]);
                QCOMPARE(request.locationSeq.value_or(0), seq);
                QVERIFY(std::abs(request.currentPosition.longitudeDeg - corridorLon(at(3))) < 1e-7);
                QVERIFY(request.issuedAtMs != sample(seq, 3, parallelLat, corridorLon(at(3))).timestampMs);
            }
        }
        QVERIFY2(await([&] { return results.count() == 1; }), qPrintable(s.flow.status()));
        const auto result = qvariant_cast<contracts::RoutePlanResult>(results[0][0]);
        QVERIFY2(result.route, qPrintable(results[0][1].toString()));
        QCOMPARE(requests.count(), 1);
        QCOMPARE(s.flow.liveNavigation().routeRevision, std::uint64_t(2));
        QVERIFY(!s.flow.liveNavigation().rerouting);
        const auto route = s.flow.state().route;
        QVERIFY(std::abs(route.first().x() - corridorLon(at(4))) < 1e-7);
        QVERIFY(std::abs(route[1].x() - corridorLon(step == 100 ? 100 : 50)) < 1e-7);
        QCOMPARE(route.last(), originalDestination);
        const double expected = data::osmxDistanceMeters(parallelLat, corridorLon(at(4)), parallelLat, corridorLon(finish)) +
            data::osmxDistanceMeters(parallelLat, corridorLon(finish), routeLat, corridorLon(finish));
        QVERIFY(std::abs(result.route->totalDistanceM - expected) < 0.2);
        qInfo() << "entry" << at(4) << "next node" << route[1] << "remaining m" << result.route->totalDistanceM
                << "elapsed ms" << clock.elapsed() << "requests" << requests.count();
    }

    void cancelledRerouteRetainsPhysicalSlot() {
        Scenario s;
        QVERIFY(s.load(orderedCorridor(10, 200), corridorLon(200)));
        const auto original = s.flow.state().route;
        // Keep off-route observations out of the confirmation window.
        s.options.windowSize = 2;
        const auto ready = std::make_shared<QSemaphore>();
        const auto allow = std::make_shared<QSemaphore>();
        s.options.rerouteResultReady = [ready, allow] {
            ready->release();
            allow->tryAcquire(1, 9000);
        };
        QVERIFY(s.start());
        QSignalSpy requests(&s.flow, &app::NavigationWorkflow::navigationRerouteRequested);
        QSignalSpy results(&s.flow, &app::NavigationWorkflow::navigationRerouteFinished);
        s.push(-1, parallelLat, corridorLon(1));
        QVERIFY(s.waiting());
        for (int time = 0; time < 4; ++time) QVERIFY(s.match(time, parallelLat, corridorLon(32)));
        QCOMPARE(requests.count(),1);
        QVERIFY(await([&] { return ready->available() == 1; }));
        s.flow.pauseNavigation();
        QVERIFY(!s.flow.liveNavigation().rerouting);
        QVERIFY(s.flow.navigationTaskCount() >= 1);
        allow->release();
        QVERIFY(await([&] { return s.flow.navigationTaskCount() == 0; }));
        QCOMPARE(results.count(), 0);
        QCOMPARE(s.flow.state().route, original);
        s.flow.stopNavigation();
        QVERIFY(s.start());
        QCOMPARE(s.flow.liveNavigation().routeRevision,std::uint64_t(1));
    }

    void sixSecondIntegrity_data() {
        QTest::addColumn<bool>("corrupt");
        QTest::newRow("healthy") << false;
        QTest::newRow("same-size-same-mtime-dbf-change") << true;
    }

    void sixSecondIntegrity() {
        QFETCH(bool, corrupt);
        Scenario s;
        QVERIFY(s.load());
        const auto marker = s.dir.filePath("fmm-networks");
        QVERIFY(recordNetworks(s, marker));
        const auto calls = std::make_shared<std::atomic<int>>(0);
        s.options.resourceAuditIntervalMs = 350;
        s.options.resourceAuditStarted = [calls] {
            if (++*calls == 1) QThread::msleep(6000);
        };
        QVERIFY(s.start());
        const auto generation = s.flow.liveNavigation().resourceGeneration;
        const auto route = s.flow.state().route;
        s.push(1, routeLat, 114.3005);
        QVERIFY(s.waiting());
        QVERIFY(s.match(2, routeLat, 114.301));
        const auto original = readLines(marker).value(0);
        QVERIFY(original.endsWith(".shp"));
        if (corrupt) {
            const auto dbfPath = QString::fromUtf8(original.chopped(4) + ".dbf");
            QFile dbf(dbfPath);
            QVERIFY(dbf.open(QIODevice::ReadWrite));
            const auto timestamp = dbf.fileTime(QFileDevice::FileModificationTime);
            auto bytes = dbf.readAll();
            QVERIFY(bytes.size() > 203);
            const auto first = bytes.mid(130, 12), second = bytes.mid(167, 12);
            QVERIFY(first != second);
            bytes.replace(130, 12, second);
            bytes.replace(167, 12, first);
            QVERIFY(dbf.seek(0));
            QCOMPARE(dbf.write(bytes), bytes.size());
            QVERIFY(dbf.flush());
            QVERIFY(dbf.setFileTime(timestamp, QFileDevice::FileModificationTime));
            dbf.close();
            QCOMPARE(QFileInfo(dbfPath).lastModified(), timestamp);
            QCOMPARE(QFileInfo(dbfPath).size(), qint64(bytes.size()));
        }
        QTest::qWait(400);
        s.push(3, routeLat, 114.3015);
        QVERIFY(await([&] { return calls->load() == 1; }));
        // Keep feeding at 1 Hz with the usual GPS gap policy. An old window can
        // discover damage to the current resource without publishing its fix.
        for (int second = 4; second <= 8; ++second) {
            QTest::qWait(1000);
            s.push(second, routeLat, 114.3015 + (second - 3) * 0.0002);
        }
        const auto newest = s.seq - 1;
        if (corrupt) {
            QVERIFY2(await([&] { return s.flow.liveNavigation().resourceGeneration == 0; }, 9000),
                qPrintable(s.flow.status()));
            QVERIFY(s.flow.status().contains("SHA-256"));
            QCOMPARE(readLines(marker).count(original),1);
            QVERIFY(s.flow.liveNavigation().resourceScanMs >= 6000);
            QTest::qWait(1100);
            s.push(9, routeLat, 114.3002);
            QVERIFY(s.waiting());
            QVERIFY(s.match(10, routeLat, 114.3048));
            QVERIFY(s.flow.liveNavigation().resourceGeneration > generation);
            QCOMPARE(s.flow.liveNavigation().preparationCount,std::size_t(2));
        } else {
            QVERIFY2(await([&] {
                const auto& current = s.flow.liveNavigation().match;
                return current && current->locationSeq == newest;
            }, 9000), qPrintable(s.flow.status()));
            QCOMPARE(s.flow.liveNavigation().resourceGeneration,generation);
            QCOMPARE(s.flow.liveNavigation().preparationCount,std::size_t(1));
            QVERIFY(readLines(marker).count(original) >= 2);
            QVERIFY(s.flow.liveNavigation().resourceScanMs >= 6000);
        }
        QCOMPARE(s.flow.state().route, route);
        QVERIFY(s.flow.liveNavigation().matchedEdge);
        const auto roads = s.flow.state().network->roads;
        const auto matchEdge = roads->edgeAt(quint32(s.flow.liveNavigation().matchedEdge->value - 1));
        QCOMPARE(roads->wayAt(matchEdge.way).id, qint64(100));
        qInfo() << "audit ms" << s.flow.liveNavigation().resourceScanMs << "corrupt" << corrupt
                << "asset gen" << generation << s.flow.liveNavigation().resourceGeneration
                << "fmm uses" << readLines(marker);
    }



    void incomingEdgeRemainsOnRoute_data() {
        QTest::addColumn<int>("position");
        QTest::newRow("75m-with-25m-remaining") << 75;
        QTest::newRow("55m-with-45m-remaining") << 55;
    }

    void incomingEdgeRemainsOnRoute() {
        QFETCH(int, position);
        Scenario s;
        constexpr int junction = 100, finish = 200;
        QVERIFY(s.load(orderedCorridor(junction, finish), corridorLon(finish)));
        s.options.windowSize = 3;
        s.options.rerouteResultReady = [] { QThread::msleep(1300); };
        QVERIFY(s.start());
        const auto original = s.flow.state().route;
        const auto originalId = s.flow.liveNavigation().routeId;
        const auto version = s.flow.liveNavigation().networkVersion;
        const auto session = s.flow.liveNavigation().sessionId;
        QSignalSpy requests(&s.flow, &app::NavigationWorkflow::navigationRerouteRequested);
        QSignalSpy results(&s.flow, &app::NavigationWorkflow::navigationRerouteFinished);
        const auto lon = corridorLon(position);
        constexpr double rawLat = parallelLat + 0.000015;
        QElapsedTimer clock;
        clock.start();
        s.push(-1, rawLat, lon);
        QVERIFY(s.waiting());
        for (int second = 0; second <= 4; ++second) {
            QVERIFY(await([&] { return clock.elapsed() >= 1000 * second; }));
            QVERIFY2(s.match(second, rawLat, lon), qPrintable(s.flow.status()));
        }
        const auto confirmed = s.flow.liveNavigation().match->matched;
        const auto matchedEdge = s.flow.liveNavigation().matchedEdge;
        QVERIFY(confirmed && matchedEdge);
        QVERIFY(std::abs(confirmed->latitudeDeg - rawLat) > 1e-6);
        const auto network = s.flow.state().network;
        const auto edge = network->roads->edgeAt(quint32(matchedEdge->value - 1));
        QVERIFY(std::abs(network->roads->nodeAt(edge.from).lon - corridorLon(0)) < 1e-7);
        QVERIFY(std::abs(network->roads->nodeAt(edge.to).lon - corridorLon(junction)) < 1e-7);
        QVERIFY2(await([&] { return results.count() == 1; }), qPrintable(s.flow.status()));
        QCOMPARE(requests.count(), 1);
        const auto request = qvariant_cast<contracts::RerouteRequest>(requests[0][0]);
        QCOMPARE(request.previousRouteId, originalId);
        const auto first = qvariant_cast<contracts::RoutePlanResult>(results[0][0]);
        const auto applied = s.flow.state().route;
        const auto geometry = s.flow.state().routeGeometry;
        const auto appliedId = s.flow.liveNavigation().routeId;
        const auto appliedRevision = s.flow.liveNavigation().routeRevision;
        const auto entry = network->project({confirmed->longitudeDeg, confirmed->latitudeDeg});
        const auto entryToRoute = algorithms::measureRouteProgress(*geometry, {entry.x(), entry.y()});
        QVERIFY(entryToRoute);
        const auto metresFromU = [](double longitude) { return (longitude - corridorLon(0)) / 0.00001044; };
        const double expectedTail = data::osmxDistanceMeters(parallelLat, lon, parallelLat, corridorLon(junction));
        const double expectedDistance = data::osmxDistanceMeters(parallelLat, lon, parallelLat, corridorLon(finish)) +
            data::osmxDistanceMeters(parallelLat, corridorLon(finish), routeLat, corridorLon(finish));
        qInfo() << "stop x" << position << "success" << bool(first.route)
                << "first x" << metresFromU(applied.first().x()) << "next x" << metresFromU(applied[1].x())
                << "first y" << applied.first().y() << "distance m" << s.flow.state().distanceM
                << "expected m" << expectedDistance << "matched distance to route m" << entryToRoute->distanceM
                << "revision" << appliedRevision << "error" << results[0][1].toString();
        QVERIFY2(first.route, qPrintable(results[0][1].toString()));

        // Keep the same valid FMM position beyond the 10 s cooldown from t=3.
        // The first matched window on the replacement route must include the tail.
        double firstMatchedDistance = -1;
        for (int second = 5; second <= 15; ++second) {
            QVERIFY(await([&] { return clock.elapsed() >= 1000 * second; }));
            QVERIFY2(s.match(second, rawLat, lon), qPrintable(s.flow.status()));
            const auto distance = s.flow.liveNavigation().match->distanceToRouteM;
            QVERIFY(distance);
            if (firstMatchedDistance < 0) firstMatchedDistance = *distance;
        }
        const auto& live = s.flow.liveNavigation();
        qInfo() << "stop x" << position << "first post-apply deviation m" << firstMatchedDistance
                << "requests after cooldown" << requests.count() << "results" << results.count()
                << "revision after cooldown" << live.routeRevision << "expected incoming tail m" << expectedTail;
        QVERIFY(std::abs(applied.first().x() - confirmed->longitudeDeg) < 1e-7);
        QVERIFY(std::abs(applied.first().y() - confirmed->latitudeDeg) < 1e-7);
        QVERIFY(std::abs(applied[1].x() - corridorLon(junction)) < 1e-7);
        QVERIFY(std::abs(applied[1].y() - parallelLat) < 1e-7);
        QCOMPARE(applied.last(), original.last());
        QVERIFY(std::hypot(geometry->front().x_m - entry.x(), geometry->front().y_m - entry.y()) < 1e-5);
        const auto& next = (*geometry)[1];
        const double publishedTail = std::hypot(next.x_m - entry.x(), next.y_m - entry.y());
        qInfo() << "stop x" << position << "published incoming tail m" << publishedTail
                << "directed legs" << s.flow.state().count;
        QVERIFY(std::abs(publishedTail - expectedTail) < 0.2);
        QVERIFY(entryToRoute->distanceM < 1e-5);
        QVERIFY(firstMatchedDistance < 0.2);
        QVERIFY(std::abs(first.route->totalDistanceM - expectedDistance) < 0.2);
        QCOMPARE(s.flow.state().distanceM, first.route->totalDistanceM);
        QCOMPARE(s.flow.state().count, 3);
        QCOMPARE(first.route->routeId, appliedId);
        QCOMPARE(first.route->geometryRef, appliedId);
        QCOMPARE(first.route->dataVersion, version);
        QCOMPARE(appliedRevision, std::uint64_t(2));
        QCOMPARE(live.routeId, appliedId);
        QCOMPARE(live.routeRevision, appliedRevision);
        QCOMPARE(live.networkVersion, version);
        QCOMPARE(live.sessionId, session);
        QVERIFY(!live.rerouting);
        QCOMPARE(requests.count(), 1);
        QCOMPARE(results.count(), 1);
        QCOMPARE(s.flow.state().route, applied);
    }

    void candidateWaitsForLatest_data() {
        QTest::addColumn<int>("holdMs");
        QTest::newRow("confirmed-after-astar") << 800;
        QTest::newRow("confirmation-after-deadline") << 3100;
    }

    void candidateWaitsForLatest() {
        QFETCH(int, holdMs);
        Scenario s;
        QVERIFY(s.load(orderedCorridor(10, 200), corridorLon(200)));
        const auto gate = s.dir.filePath("delay");
        const auto wrapper = s.dir.filePath("fmm-delay");
        QVERIFY(writeFile(wrapper, "#!/bin/sh\n[ ! -f " + shellQuote(gate) + " ] || sleep " +
            QByteArray::number(holdMs / 1000.0) + "\nexec " + shellQuote(s.options.fmmExecutable) + " \"$@\"\n"));
        QVERIFY(QFile::setPermissions(wrapper,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
        s.options.fmmExecutable = wrapper;
        s.options.rerouteResultReady = [] { QThread::msleep(400); };
        QVERIFY(s.start());
        QSignalSpy results(&s.flow, &app::NavigationWorkflow::navigationRerouteFinished);
        s.push(-1, parallelLat, corridorLon(1));
        QVERIFY(s.waiting());
        for (int second = 0; second < 4; ++second)
            QVERIFY2(s.match(second, parallelLat, corridorLon(2 + 10 * second)), qPrintable(s.flow.status()));
        QVERIFY(s.flow.liveNavigation().rerouting);
        QVERIFY(writeFile(gate,"on"));
        s.push(4, parallelLat, corridorLon(42));
        QVERIFY(await([&] { return s.flow.navigationTaskCount() == 1 && s.flow.liveNavigation().rerouting; }));
        QVERIFY(!s.flow.liveNavigation().match);
        QCOMPARE(s.flow.liveNavigation().routeRevision,std::uint64_t(1));
        QVERIFY2(await([&] { return results.count() == 1; }), qPrintable(s.flow.status()));
        const auto route = qvariant_cast<contracts::RoutePlanResult>(results[0][0]).route;
        if (holdMs < 2000) {
            QVERIFY2(route, qPrintable(results[0][1].toString()));
            QVERIFY(std::abs(s.flow.state().route.first().x() - corridorLon(42)) < 1e-7);
        } else {
            QVERIFY(!route);
            QVERIFY(results[0][1].toString().contains("超时"));
            QVERIFY(!s.flow.liveNavigation().rerouting);
            QVERIFY(await([&] { return s.flow.navigationTaskCount() == 0; }));
            QCOMPARE(s.flow.liveNavigation().routeRevision,std::uint64_t(1));
            QCOMPARE(results.count(),1);
        }
    }

    void incompatibleCandidate_data() {
        QTest::addColumn<QString>("cause");
        QTest::newRow("return-to-original-route") << QString("return");
        QTest::newRow("confirmed-reverse-edge") << QString("reverse");
        QTest::newRow("expired-worker") << QString("timeout");
    }

    void incompatibleCandidate() {
        QFETCH(QString, cause);
        Scenario s;
        constexpr int finish = 200;
        QVERIFY(s.load(orderedCorridor(10, finish, cause == "reverse"), corridorLon(finish)));
        const auto original = s.flow.state().route;
        const auto ready = std::make_shared<QSemaphore>();
        const auto allow = std::make_shared<QSemaphore>();
        s.options.windowSize = 2;
        s.options.rerouteResultReady = [ready, allow] {
            ready->release();
            allow->tryAcquire(1, 9000);
        };
        if (cause == "timeout") s.options.rerouteTimeoutMs = 300;
        QVERIFY(s.start());
        QSignalSpy results(&s.flow, &app::NavigationWorkflow::navigationRerouteFinished);
        const int base = cause == "return" ? 162 : 2;
        s.push(-1, parallelLat, corridorLon(base - 1));
        QVERIFY(s.waiting());
        for (int second = 0; second < 4; ++second)
            QVERIFY2(s.match(second, parallelLat, corridorLon(base + 10 * second)), qPrintable(s.flow.status()));
        QVERIFY(await([&] { return ready->available() == 1; }));
        if (cause == "return") {
            QVERIFY2(s.match(4, routeLat, corridorLon(finish)), qPrintable(s.flow.status()));
            QVERIFY(s.flow.liveNavigation().match->distanceToRouteM.value() < 15.0);
            QVERIFY(await([&] { return !results.isEmpty(); }));
            QVERIFY(results[0][1].toString().contains("已回到原路线"));
            QVERIFY(!s.flow.liveNavigation().rerouting);
            QVERIFY(s.flow.navigationTaskCount() >= 1);
        } else if (cause == "reverse") {
            QVERIFY(s.match(4, parallelLat, corridorLon(21)));
            const auto p0 = s.flow.liveNavigation().matchedEdge;
            QVERIFY(p0);
            const auto roads = s.flow.state().network->roads;
            const auto edge = roads->edgeAt(quint32(p0->value - 1));
            QVERIFY(roads->nodeAt(edge.from).lon > roads->nodeAt(edge.to).lon);
        } else {
            QVERIFY(await([&] { return !results.isEmpty(); }));
            QVERIFY(results[0][1].toString().contains("超时"));
            QVERIFY(!s.flow.liveNavigation().rerouting);
            QVERIFY(s.flow.navigationTaskCount() >= 1);
        }
        allow->release();
        QVERIFY(await([&] { return s.flow.navigationTaskCount() == 0; }));
        QCOMPARE(results.count(), 1);
        QVERIFY(!qvariant_cast<contracts::RoutePlanResult>(results[0][0]).route);
        QCOMPARE(s.flow.state().route, original);
        QCOMPARE(s.flow.liveNavigation().routeRevision, std::uint64_t(1));
        qInfo() << "rejection" << cause << results[0][1].toString();
    }

    void initialPreparationRejected_data() {
        QTest::addColumn<QByteArray>("row");
        QTest::newRow("only-header") << QByteArray("");
        QTest::newRow("partial-row") << QByteArray("0;1;1;0;");
        QTest::newRow("outside-edge-index") << QByteArray("0;1;1;0;300000;100\n");
        QTest::newRow("wrong-directed-edge") << QByteArray("0;1;1;0;8;100\n");
    }

    void initialPreparationRejected() {
        QFETCH(QByteArray, row);
        Scenario s;
        QVERIFY(s.load());
        const auto marker = s.dir.filePath("fmm-launched");
        QVERIFY(recordNetworks(s, marker));
        const auto generator = s.dir.filePath("bad-generator");
        QVERIFY(writeFile(generator, "#!/bin/sh\nprev=\nfor arg do\n"
            "  if [ \"$prev\" = --output ]; then cat > \"$arg\" <<'TABLE'\n"
            "source;target;next_n;prev_n;next_e;distance\n" + row +
            (row.isEmpty() || row.endsWith('\n') ? "" : "\n") +
            "TABLE\n    exit 0\n  fi\n  prev=$arg\ndone\nexit 1\n"));
        QVERIFY(QFile::setPermissions(generator,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
        s.options.ubodtExecutable = generator;
        QVERIFY(s.flow.startNavigation(nullptr, s.options));
        QVERIFY(await([&] { return s.flow.liveNavigation().matchStatus == FmmStatus::InvalidConfiguration; }));
        QCOMPARE(s.flow.liveNavigation().resourceGeneration, quint64(0));
        QCOMPARE(s.flow.liveNavigation().preparationCount,std::size_t(1));
        QVERIFY(s.flow.status().contains("UBODT"));
        QVERIFY(!QFileInfo::exists(marker));
        qInfo() << "initial publish refused" << s.flow.status();
    }

    void legalEmptyTableAndManualMatching() {
        Scenario s;
        QVERIFY(s.load());
        const auto network = s.flow.state().network;
        auto sparse = network->graph;
        sparse.edges.resize(1);
        auto& edge = sparse.edges.front();
        edge.geometry.back().x_m = edge.geometry.front().x_m + 2300.0;
        edge.geometry.back().y_m = edge.geometry.front().y_m;
        edge.length_m = edge.cost_m = 2300.0;
        sparse.nodes[edge.to.value - 1].position = edge.geometry.back();
        const auto prepared = app::prepareMatchingNetwork(*network, sparse, network->version,
            std::make_shared<std::atomic_bool>(false), s.options.fmmExecutable, s.options.ubodtExecutable);
        QFile table(prepared.config.ubodtFile);
        QVERIFY(table.open(QIODevice::ReadOnly));
        QCOMPARE(table.readAll().trimmed(), QByteArray("source;target;next_n;prev_n;next_e;distance"));
        QVERIFY(prepared.config.resources);

        const auto csv = s.dir.filePath("manual.csv");
        QVERIFY(writeFile(csv, "latitude,longitude\n30.593,114.3004\n30.593,114.3020\n30.593,114.3047\n"));
        QSignalSpy done(&s.flow, &app::NavigationWorkflow::completed);
        s.flow.match(csv);
        QVERIFY2(await([&] { return done.count() == 1; }), qPrintable(s.flow.status()));
        QVERIFY2(done[0][1].toBool(), qPrintable(s.flow.status()));
        QVERIFY(s.flow.state().matched.size() > 2);
        qInfo() << "legal empty table and shared preparation for manual match" << s.flow.status();
    }

    void preparationBudgetRecovers() {
        Scenario s;
        QVERIFY(s.load());
        s.options.fixGapMs = 20000;
        const auto wrapper = s.dir.filePath("unhealthy-then-real-generator");
        const auto real = s.options.ubodtExecutable;
        QVERIFY(writeFile(wrapper, "#!/bin/sh\nexit 8\n"));
        QVERIFY(QFile::setPermissions(wrapper,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
        s.options.ubodtExecutable = wrapper;
        const auto route = s.flow.state().route;
        QVERIFY(s.flow.startNavigation(nullptr, s.options));
        for (int attempt = 1; attempt <= 3; ++attempt) {
            QVERIFY(await([&] { return s.flow.liveNavigation().preparationCount == std::size_t(attempt) &&
                s.flow.liveNavigation().matchStatus == FmmStatus::ExecutionFailed && s.flow.navigationTaskCount() == 0; }));
            if (attempt != 3) {
                s.push(attempt * 2, routeLat, 114.301);
                QTest::qWait(attempt == 1 ? 1100 : 5100);
                QCOMPARE(s.flow.liveNavigation().preparationCount, std::size_t(attempt));
                s.push(attempt * 2 + 1, routeLat, 114.3011);
            }
        }
        QVERIFY(s.flow.status().contains("次数已用完"));
        s.flow.pauseNavigation();
        s.flow.resumeNavigation();
        QTest::qWait(1100);
        s.push(8, routeLat, 114.302);
        QCOMPARE(s.flow.liveNavigation().preparationCount,std::size_t(3));
        QCOMPARE(s.flow.navigationTaskCount(),0);
        QCOMPARE(s.flow.state().route,route);
        QVERIFY(writeFile(wrapper, "#!/bin/sh\nexec " + shellQuote(real) + " \"$@\"\n"));
        s.flow.stopNavigation();
        QVERIFY(s.start());
        s.push(20,routeLat,114.3025);
        QVERIFY(s.waiting());
        QVERIFY2(s.match(21,routeLat,114.3035),qPrintable(s.flow.status()));
        QCOMPARE(s.flow.liveNavigation().preparationCount,std::size_t(1));
    }

    void integrityBudgetStops() {
        Scenario s;
        QVERIFY(s.load());
        const auto calls = std::make_shared<std::atomic<int>>(0);
        const auto marker = s.dir.filePath("fmm-launched");
        QVERIFY(recordNetworks(s, marker));
        s.options.fixGapMs = 15000;
        s.options.resourceAuditIntervalMs = 350;
        s.options.resourceAuditBudgetMs = 80;
        s.options.resourceAuditStarted = [calls] {
            ++*calls;
            QThread::msleep(150);
        };
        QVERIFY(s.start());
        s.push(1, routeLat, 114.3005);
        QVERIFY(s.waiting());
        QVERIFY(s.match(2, routeLat, 114.301));
        const auto generation = s.flow.liveNavigation().resourceGeneration;
        QTest::qWait(400);
        const auto failed = [&] {
            return s.flow.navigationTaskCount() == 0 && s.flow.liveNavigation().matchStatus == FmmStatus::ExecutionFailed;
        };
        s.push(3, routeLat, 114.3015);
        QVERIFY(await(failed));
        QCOMPARE(calls->load(),1);
        QVERIFY(s.flow.status().contains("总时间预算"));
        s.push(4, routeLat, 114.302);
        QVERIFY(await(failed));
        QCOMPARE(calls->load(),1);
        QTest::qWait(5100);
        s.push(5, routeLat, 114.3025);
        QVERIFY(await(failed));
        QCOMPARE(calls->load(),2);
        s.push(6, routeLat, 114.303);
        QVERIFY(await(failed));
        QVERIFY(s.flow.status().contains("预算已用完"));
        QCOMPARE(calls->load(),2);
        QCOMPARE(readLines(marker).size(),1);
        QCOMPARE(s.flow.liveNavigation().resourceGeneration,generation);
        QCOMPARE(s.flow.liveNavigation().preparationCount,std::size_t(1));
        s.flow.stopNavigation();
        s.options.resourceAuditStarted = {};
        s.options.resourceAuditBudgetMs = 30000;
        QVERIFY(s.start());
        s.push(7, routeLat, 114.3035);
        QVERIFY(s.waiting());
        QVERIFY(s.match(8, routeLat, 114.304));
        QCOMPARE(readLines(marker).size(),2);
    }


    void routeGeometry() {
        using algorithms::measureRouteProgress;
        const std::vector<algorithms::model::MetricPoint> route{{0,0},{0,0},{3,0},{3,4}};
        const auto middle = measureRouteProgress(route, {4,2});
        QVERIFY(middle);
        QCOMPARE(middle->distanceM,1.0);
        QCOMPARE(middle->alongM,5.0);
        QCOMPARE(measureRouteProgress(route, {-2,0})->alongM,0.0);
        QCOMPARE(measureRouteProgress(route, {4,9})->alongM,7.0);
        QVERIFY(measureRouteProgress({{0,0},{1e-180,0}}, {1e-181,0})->alongM > 0);
        QVERIFY(!measureRouteProgress({{1,1},{1,1}}, {3,3}));
        QVERIFY(!measureRouteProgress(route, {std::numeric_limits<double>::infinity(), 0}));
        QVERIFY(!measureRouteProgress({{0,0},{std::numeric_limits<double>::quiet_NaN(),0}}, {0,0}));
    }

    void invalidStart() {
        app::NavigationWorkflow flow;
        QVERIFY(!flow.startNavigation());
        QVERIFY(!flow.liveNavigation().active);
        QCOMPARE(flow.navigationTaskCount(),0);
    }

    void replayWindowAndGap() {
        Scenario s;
        QVERIFY2(s.load(), qPrintable(s.flow.status()));
        s.options.windowSize = 3;
        s.options.fixGapMs = 2200;
        navigation::FixedTrack track;
        track.dataVersion = "phase3-replay";
        for (int i = 0; i < 9; ++i) track.frames.push_back(sample(i, i, routeLat, 114.3002 + 0.00035*i));
        navigation::MockLocationSource source;
        QVERIFY(source.setTrack(std::move(track)));
        QVERIFY2(s.start(&source),qPrintable(s.flow.status()));
        const auto session = s.flow.liveNavigation().sessionId;
        source.start();
        QVERIFY(source.pause());
        QVERIFY(source.stepOnce());
        QVERIFY(s.waiting());
        QCOMPARE(s.flow.liveNavigation().windowPoints, std::size_t(1));
        for (quint64 seq = 1; seq <= 3; ++seq) {
            QVERIFY(source.stepOnce());
            QVERIFY2(await([&] {
                const auto& live = s.flow.liveNavigation();
                return live.match && live.match->locationSeq == seq;
            }), qPrintable(s.flow.status()));
        }
        const auto& live = s.flow.liveNavigation();
        QCOMPARE(live.windowPoints, std::size_t(3));
        QVERIFY(live.match->distanceToRouteM && *live.match->distanceToRouteM < 1.0);
        QVERIFY(live.match->alongRouteDistanceM && *live.match->alongRouteDistanceM > 80.0);
        QVERIFY(live.matchedEdge);
        const auto index = quint32(live.matchedEdge->value - 1);
        const auto roads = s.flow.state().network->roads;
        QCOMPARE(roads->wayAt(roads->edgeAt(index).way).id, qint64(100));
        QVERIFY(live.networkVersion.endsWith("/car"));
        QCOMPARE(live.preparationCount,std::size_t(1));
        s.flow.locationUpdated(sample(3, 99, parallelLat, 114.301, source.sessionId()));
        QCOMPARE(live.match->locationSeq, std::uint64_t(3));
        auto stale = sample(2, 99, parallelLat, 114.301, source.sessionId());
        stale.coordinate.longitudeDeg = std::numeric_limits<double>::quiet_NaN();
        s.flow.locationUpdated(stale);
        QCOMPARE(live.match->locationSeq, std::uint64_t(3));
        s.flow.locationUpdated(sample(4, 2, routeLat, 114.301, source.sessionId()));
        QCOMPARE(live.matchStatus, std::optional<FmmStatus>(FmmStatus::InvalidInput));

        s.flow.pauseNavigation();
        QVERIFY(source.stepOnce());
        QVERIFY(live.paused);
        QVERIFY(!live.match);
        s.flow.resumeNavigation();
        QVERIFY(source.stepOnce());
        QVERIFY(s.waiting());
        QVERIFY2(await([&]{return live.locationLost;}, 4200),qPrintable(s.flow.status()));
        QVERIFY(source.stepOnce());
        QVERIFY(s.waiting());
        QVERIFY(source.stepOnce());
        QVERIFY2(await([&]{ return live.match.has_value(); }),qPrintable(s.flow.status()));
        QCOMPARE(live.preparationCount,std::size_t(1));
        s.flow.stopNavigation();
        QCOMPARE(live.matchStatus,std::optional<FmmStatus>(FmmStatus::Cancelled));
        QVERIFY(s.start());
        QVERIFY(live.sessionId > session);
        s.push(1, routeLat, 114.301);
        QVERIFY(s.waiting());
        QVERIFY(!live.match);
    }

    void deviationAndReroute() {
        Scenario s;
        QVERIFY2(s.load(),qPrintable(s.flow.status()));
        s.options.windowSize = 3;
        QVERIFY2(s.start(),qPrintable(s.flow.status()));
        QSignalSpy requests(&s.flow, &app::NavigationWorkflow::navigationRerouteRequested);
        QSignalSpy results(&s.flow, &app::NavigationWorkflow::navigationRerouteFinished);
        QTimer heartbeat;
        int ticks = 0;
        connect(&heartbeat, &QTimer::timeout, [&] { ++ticks; });
        heartbeat.start(20);

        s.push(1,routeLat,114.3005);
        QVERIFY(s.waiting());
        QVERIFY2(s.match(2,routeLat,114.3015),qPrintable(s.flow.status()));
        QVERIFY(*s.flow.liveNavigation().match->distanceToRouteM < 1.0);
        QCOMPARE(requests.count(),0);

        // One isolated offset followed by a gap cannot satisfy the confirmed three-second rule.
        s.push(10,parallelLat,114.301);
        QVERIFY(s.waiting());
        QVERIFY2(s.match(11,parallelLat,114.3015),qPrintable(s.flow.status()));
        QCOMPARE(requests.count(),0);
        s.push(20,parallelLat,114.301);
        QVERIFY(s.waiting());
        QVERIFY2(s.match(21,parallelLat,114.30145),qPrintable(s.flow.status()));
        QVERIFY2(s.match(22,parallelLat,114.3019),qPrintable(s.flow.status()));
        QVERIFY2(s.match(23,parallelLat,114.30235),qPrintable(s.flow.status()));
        QCOMPARE(requests.count(),0);
        const auto version = s.flow.liveNavigation().networkVersion;
        const auto oldRoute = s.flow.liveNavigation().routeId;
        s.push(24,parallelLat,114.3028);
        QVERIFY2(await([&]{return results.count() == 1;}),qPrintable(s.flow.status()));
        QCOMPARE(requests.count(),1);
        const auto request = qvariant_cast<contracts::RerouteRequest>(requests[0][0]);
        QCOMPARE(request.destination.longitudeDeg,114.306);
        QCOMPARE(request.currentPosition.latitudeDeg,parallelLat);
        const auto result = qvariant_cast<contracts::RoutePlanResult>(results[0][0]);
        QVERIFY2(result.route.has_value(),qPrintable(results[0][1].toString()));
        QCOMPARE(result.requestId,request.requestId);
        QVERIFY(result.route->routeId != oldRoute);
        QCOMPARE(result.route->dataVersion,version);
        QCOMPARE(s.flow.liveNavigation().routeRevision,std::uint64_t(2));
        QCOMPARE(s.flow.liveNavigation().preparationCount,std::size_t(1));
        QVERIFY(std::abs(s.flow.state().route.first().y()-parallelLat) < 1e-7);
        QVERIFY(!s.flow.liveNavigation().match);
        showMeasurements(s.flow,ticks);
        QVERIFY(ticks > 0);
    }

    void unreachableCooldown() {
        Scenario s;
        QVERIFY(s.load());
        QVERIFY2(s.start(),qPrintable(s.flow.status()));
        const auto preserved = s.flow.state().route;
        QSignalSpy requests(&s.flow,&app::NavigationWorkflow::navigationRerouteRequested);
        QSignalSpy results(&s.flow,&app::NavigationWorkflow::navigationRerouteFinished);
        s.push(1,isolatedLat,114.301);
        QVERIFY(s.waiting());
        for (int second = 2; second <= 4; ++second)
            QVERIFY2(s.match(second,isolatedLat,114.301 + 0.0002*second),qPrintable(s.flow.status()));
        s.push(5,isolatedLat,114.302);
        QVERIFY2(await([&]{return results.count() == 1;}),qPrintable(s.flow.status()));
        QVERIFY(!qvariant_cast<contracts::RoutePlanResult>(results[0][0]).route);
        QVERIFY(!results[0][1].toString().isEmpty());
        QCOMPARE(s.flow.state().route,preserved);
        for (int second = 6; second <= 14; ++second)
            QVERIFY2(s.match(second,isolatedLat,114.302 + 0.00015*(second-6)),qPrintable(s.flow.status()));
        QCOMPARE(requests.count(),1);
        s.push(15,isolatedLat,114.3034);
        QVERIFY2(await([&]{return results.count() == 2;}),qPrintable(s.flow.status()));
        QCOMPARE(requests.count(),2);
        QCOMPARE(s.flow.state().route,preserved);
    }

    void recoverBackends() {
        Scenario s;
        QVERIFY(s.load());
        const auto route = s.flow.state().route;
        const auto realFmm = s.options.fmmExecutable;
        const auto realGen = s.options.ubodtExecutable;
        const auto mode = s.dir.filePath("mode");
        const auto tablePath = s.dir.filePath("generated-table-path");
        const auto fmm = s.dir.filePath("fmm-wrapper");
        const auto generator = s.dir.filePath("ubodt-wrapper");
        QVERIFY(writeFile(mode,"fail-gen"));
        QVERIFY(writeFile(generator,"#!/bin/sh\n[ \"$(cat " + shellQuote(mode) + ")\" = fail-gen ] && exit 8\nexec " +
            shellQuote(realGen) + " \"$@\"\n"));
        QVERIFY(writeFile(fmm,"#!/bin/sh\nlast=\nfor arg do\n"
            "  if [ \"$last\" = --ubodt ]; then printf '%s\\n' \"$arg\" > " + shellQuote(tablePath) +
            "; break; fi\n  last=$arg\ndone\ncase \"$(cat " + shellQuote(mode) +
            ")\" in fail) exit 9;; timeout) exec sleep 5;; empty) exit 0;; esac\nexec " +
            shellQuote(realFmm) + " \"$@\"\n"));
        QVERIFY(QFile::setPermissions(generator,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
        QVERIFY(QFile::setPermissions(fmm,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
        s.options.ubodtExecutable = generator;
        s.options.fmmExecutable = fmm;
        s.options.matchTimeoutMs = 1000;
        QVERIFY(s.flow.startNavigation(nullptr,s.options));
        auto has = [&](FmmStatus status) {
            return await([&] { return s.flow.liveNavigation().matchStatus == status && s.flow.navigationTaskCount() == 0; });
        };
        QVERIFY2(has(FmmStatus::ExecutionFailed),qPrintable(s.flow.status()));
        QVERIFY(s.flow.status().contains("UBODT"));
        QVERIFY(writeFile(mode,"ok"));
        QTest::qWait(1080);
        s.push(1,routeLat,114.3005);
        QVERIFY(s.waiting());
        QCOMPARE(s.flow.liveNavigation().preparationCount,std::size_t(2));
        QVERIFY(writeFile(mode,"fail"));
        s.push(2,routeLat,114.3010);
        QVERIFY2(has(FmmStatus::ExecutionFailed),qPrintable(s.flow.status()));
        QVERIFY(!s.flow.liveNavigation().match);
        QVERIFY(writeFile(mode,"empty"));
        s.push(3,routeLat,114.3015);
        QVERIFY2(has(FmmStatus::InvalidOutput),qPrintable(s.flow.status()));
        QVERIFY(writeFile(mode,"timeout"));
        s.push(4,routeLat,114.3020);
        QVERIFY2(has(FmmStatus::TimedOut),qPrintable(s.flow.status()));
        QCOMPARE(s.flow.state().route,route);
        QVERIFY(writeFile(mode,"ok"));
        QVERIFY2(s.match(5,routeLat,114.3025),qPrintable(s.flow.status()));
        QCOMPARE(s.flow.liveNavigation().preparationCount,std::size_t(2));
        auto invalid = sample(s.seq++,6,routeLat,114.303);
        invalid.coordinate.longitudeDeg = std::numeric_limits<double>::quiet_NaN();
        s.flow.locationUpdated(invalid);
        QVERIFY2(has(FmmStatus::InvalidInput),qPrintable(s.flow.status()));
        s.push(7,0,0);
        QVERIFY(s.waiting());
        s.push(8,0.001,0.001);
        QVERIFY2(has(FmmStatus::NoMatch),qPrintable(s.flow.status()));
        s.push(20,routeLat,114.303);
        QVERIFY(s.waiting());
        QVERIFY2(s.match(21,routeLat,114.3034),qPrintable(s.flow.status()));
        QCOMPARE(s.flow.state().route,route);
        QFile table(tablePath);
        QVERIFY(table.open(QIODevice::ReadOnly));
        QVERIFY(QFile::remove(QString::fromUtf8(table.readAll().trimmed())));
        s.push(22,routeLat,114.3038);
        QVERIFY2(has(FmmStatus::InvalidConfiguration),qPrintable(s.flow.status()));
        QTest::qWait(5150);
        s.push(23,routeLat,114.3040);
        QVERIFY(s.waiting());
        QVERIFY2(s.match(24,routeLat,114.3044),qPrintable(s.flow.status()));
        QCOMPARE(s.flow.liveNavigation().preparationCount,std::size_t(3));
    }

    void stopWithWorkInFlight() {
        Scenario s;
        QVERIFY(s.load());
        const auto realFmm = s.options.fmmExecutable;
        const auto gate = s.dir.filePath("gate");
        const auto marker = s.dir.filePath("started");
        const auto wrapper = s.dir.filePath("fmm-gate");
        QVERIFY(writeFile(gate,"slow"));
        QVERIFY(writeFile(wrapper,"#!/bin/sh\nif [ \"$(cat " + shellQuote(gate) +
            ")\" = slow ]; then : > " + shellQuote(marker) + "; exec sleep 5; fi\nexec " +
            shellQuote(realFmm) + " \"$@\"\n"));
        QVERIFY(QFile::setPermissions(wrapper,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
        s.options.fmmExecutable = wrapper;
        QVERIFY(s.start());
        const auto originalSession = s.flow.liveNavigation().sessionId;
        s.push(1,routeLat,114.3005);
        QVERIFY(s.waiting());
        s.push(2,routeLat,114.301);
        QVERIFY(await([&] { return QFileInfo::exists(marker); }));
        for (int i = 0; i < 100; ++i) s.push(3+i,routeLat,114.301+0.00001*i);
        QCOMPARE(s.flow.navigationTaskCount(),1);
        s.flow.pauseNavigation();
        QVERIFY(await([&] { return s.flow.navigationTaskCount() == 0; }));
        QVERIFY(s.flow.liveNavigation().cancellationMs > 0);
        showMeasurements(s.flow,0);
        s.flow.resumeNavigation();
        QVERIFY(QFile::remove(marker));
        s.push(110,routeLat,114.301);
        QVERIFY(s.waiting());
        s.push(111,routeLat,114.3015);
        QVERIFY(await([&] { return QFileInfo::exists(marker); }));
        QVERIFY(writeFile(gate,"ok"));
        s.flow.stopNavigation();
        QVERIFY2(s.start(),qPrintable(s.flow.status()));
        QVERIFY(s.flow.liveNavigation().sessionId > originalSession);
        QVERIFY(!s.flow.liveNavigation().match);
        QCOMPARE(s.flow.liveNavigation().cancellationMs,0.0);
        s.push(1,parallelLat,114.301);
        QVERIFY(s.waiting());

        const auto keptRoute = s.flow.state().route;
        QSignalSpy applied(&s.flow,&app::NavigationWorkflow::navigationRerouteFinished);
        int requests = 0;
        connect(&s.flow,&app::NavigationWorkflow::navigationRerouteRequested,&s.flow,
            [&](const auto&) {
                ++requests;
                for (int i = 0; i < 20; ++i) s.push(6+i,parallelLat,114.3021+0.00001*i);
                s.flow.stopNavigation();
            });
        for (int second=2;second<=4;++second)
            QVERIFY2(s.match(second,parallelLat,114.3005+0.0003*second),qPrintable(s.flow.status()));
        s.push(5,parallelLat,114.302);
        QVERIFY2(await([&]{return requests == 1 && s.flow.navigationTaskCount() == 0;}),qPrintable(s.flow.status()));
        QCOMPARE(applied.count(),0);
        QCOMPARE(s.flow.state().route,keptRoute);
        QCOMPARE(s.flow.liveNavigation().matchStatus,std::optional<FmmStatus>(FmmStatus::Cancelled));
        showMeasurements(s.flow,0);
    }

    void missingBackendRecovers() {
        Scenario s;
        QVERIFY(s.load());
        const auto real = s.options.fmmExecutable;
        s.options.fmmExecutable = s.dir.filePath("available-later");
        QVERIFY(s.flow.startNavigation(nullptr,s.options));
        QVERIFY(await([&] { return s.flow.liveNavigation().matchStatus == FmmStatus::ExecutionFailed; }));
        const auto route = s.flow.state().route;
        QVERIFY(writeFile(s.options.fmmExecutable,"#!/bin/sh\nexec " + shellQuote(real) + " \"$@\"\n"));
        QVERIFY(QFile::setPermissions(s.options.fmmExecutable,
            QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
        QTest::qWait(1080);
        s.push(1,routeLat,114.3005);
        QVERIFY(s.waiting());
        QVERIFY2(s.match(2,routeLat,114.3015),qPrintable(s.flow.status()));
        QCOMPARE(s.flow.state().route,route);
        QCOMPARE(s.flow.liveNavigation().preparationCount,std::size_t(2));
    }
};

QTEST_GUILESS_MAIN(LiveNavigationTests)
#include "LiveNavigationTests.moc"
