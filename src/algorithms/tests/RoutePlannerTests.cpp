#include "qt/BoostRoutePlanner.h"
#include "samples/FixedRoadNetwork.h"
#include <QSemaphore>
#include <QSignalSpy>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QtTest>
#include <stdexcept>

using namespace p1::algorithms;
namespace c = p1::contracts;

namespace {
std::shared_ptr<const model::GraphSnapshot> snapshot() {
    return model::publishGraph(samples::fixedRoadNetwork()).snapshot;
}
c::RoutePlanRequest request(std::uint64_t id, bool reverse = false) {
    c::RoutePlanRequest result;
    result.requestId = id; result.dataVersion = QStringLiteral("v1");
    result.origin = {c::CoordinateSystem::WGS84, 30, reverse ? 120.001 : 120, {}};
    result.destination = {c::CoordinateSystem::WGS84, 30, reverse ? 120 : 120.001, {}};
    return result;
}
// Fixture-only georeference lookup: two named WGS84 labels map to synthetic
// nodes. This deliberately is NOT a real geographic projection.
std::optional<model::MetricPoint> fixtureProjector(const c::GeoCoordinate& point) {
    if (point.latitudeDeg == 30 && point.longitudeDeg == 120) return model::MetricPoint{0, 0};
    if (point.latitudeDeg == 30 && point.longitudeDeg == 120.001) return model::MetricPoint{6, 0};
    return {};
}
struct GateState {
    QSemaphore release;
    std::atomic_bool entered{false};
    std::atomic_bool offGuiThread{false};
    std::atomic_int calls{0};
};
struct Gate {
    std::shared_ptr<GateState> state = std::make_shared<GateState>();
    ~Gate() { state->release.release(32); } // Also releases workers on early test failure.
    CoordinateProjector projector() const {
        return [s = state, guiThread = QThread::currentThread()](const c::GeoCoordinate& point) {
            s->offGuiThread.store(QThread::currentThread() != guiThread);
            if (s->calls.fetch_add(1) == 0) {
                s->entered.store(true);
                s->release.acquire();
            }
            return fixtureProjector(point);
        };
    }
};
void finishWorkers() {
    QThreadPool::globalInstance()->waitForDone(); // Test harness only, never application code.
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}
}

class RoutePlannerTests final : public QObject {
    Q_OBJECT
private slots:
    void successPreservesContractAndPath() {
        BoostRoutePlanner planner(snapshot(), fixtureProjector, 2.0);
        QSignalSpy ready(&planner, &c::RoutePlanner::routeReady);
        QSignalSpy failed(&planner, &c::RoutePlanner::routeFailed);
        QCOMPARE(planner.requestRoute(request(1)), std::uint64_t{1});
        QCOMPARE(ready.size(), 0); // Never synchronous in requestRoute.
        QTRY_COMPARE(ready.size(), 1);
        const auto result = qvariant_cast<c::RoutePlanResult>(ready.at(0).at(0));
        QVERIFY(result.route);
        QCOMPARE(result.requestId, std::uint64_t{1});
        QCOMPARE(result.route->requestId, result.requestId);
        QCOMPARE(result.route->dataVersion, QStringLiteral("v1"));
        QCOMPARE(result.route->totalDistanceM, 6.0);
        QCOMPARE(result.route->totalDurationSec, 3.0); // Explicit 2 m/s assumption.
        QCOMPARE(result.route->source, c::SourceKind::Real);
        QCOMPARE(result.route->geometryRef, planner.geometryRef());
        QVERIFY(result.instructions.empty());
        QVERIFY(planner.lastPath());
        QCOMPARE(planner.lastPath()->nodes.size(), std::size_t{3});
        QCOMPARE(failed.size(), 0);

        const auto oldRef = planner.geometryRef();
        const auto retained = planner.pathForGeometry(oldRef);
        QVERIFY(retained);
        bool handedOver = false;
        bool oldResolvableOnDelivery = false;
        connect(&planner, &c::RoutePlanner::routeReady, this, [&](const auto& replacement) {
            if (replacement.requestId != 2) return;
            oldResolvableOnDelivery = planner.pathForGeometry(oldRef) == retained;
            // The receiver has accepted the new route, so releases the old ref.
            planner.releaseGeometry(oldRef);
            handedOver = true;
        }, Qt::QueuedConnection);
        QCOMPARE(planner.requestRoute(request(2)), std::uint64_t{2});
        QVERIFY(planner.pathForGeometry(oldRef) == retained);
        QTRY_VERIFY(handedOver);
        QCOMPARE(ready.size(), 2);
        QVERIFY(oldResolvableOnDelivery);
        QVERIFY(!planner.pathForGeometry(oldRef));
        QVERIFY(planner.geometryRef() != oldRef);
        QVERIFY(planner.pathForGeometry(planner.geometryRef()));
        QCOMPARE(retained->nodes.size(), std::size_t{3}); // Handle still owns old geometry.
    }
    void replanningPreservesDeliveredRoute_data() {
        QTest::addColumn<bool>("haveOldRoute");
        QTest::addColumn<bool>("cancelReplan");
        QTest::newRow("first-route-unreachable") << false << false;
        QTest::newRow("old-route-then-failed-replan") << true << false;
        QTest::newRow("old-route-then-cancelled-replan") << true << true;
    }
    void replanningPreservesDeliveredRoute() {
        QFETCH(bool, haveOldRoute);
        QFETCH(bool, cancelReplan);
        Gate gate;
        if (haveOldRoute) gate.state->release.release(); // Allow the initial successful route.
        BoostRoutePlanner planner(snapshot(), gate.projector(), 2.0);
        QSignalSpy ready(&planner, &c::RoutePlanner::routeReady);
        QSignalSpy failed(&planner, &c::RoutePlanner::routeFailed);
        if (haveOldRoute) {
            QCOMPARE(planner.requestRoute(request(1)), std::uint64_t{1});
            QTRY_COMPARE(ready.size(), 1);
            gate.state->calls.store(0);
            gate.state->entered.store(false);
        }
        const auto oldRef = planner.geometryRef();
        const auto retained = planner.pathForGeometry(oldRef);
        const std::uint64_t id = haveOldRoute ? 2 : 1;
        QCOMPARE(planner.requestRoute(request(id, !cancelReplan)), id);
        QTRY_VERIFY(gate.state->entered.load());
        QCOMPARE(planner.geometryRef(), oldRef);
        QVERIFY(planner.lastPath() == retained); // Old route survives even while replanning.
        if (cancelReplan) planner.cancel(id);
        gate.state->release.release(); finishWorkers();
        QCOMPARE(ready.size(), haveOldRoute ? 1 : 0);
        QCOMPARE(failed.size(), cancelReplan ? 0 : 1);
        if (!cancelReplan) {
            QCOMPARE(failed.at(0).at(0).toULongLong(), static_cast<qulonglong>(id));
            QCOMPARE(qvariant_cast<c::NavigationErrorCode>(failed.at(0).at(1)),
                     c::NavigationErrorCode::NoRouteAvailable);
        }
        QCOMPARE(planner.geometryRef(), oldRef);
        QVERIFY(planner.lastPath() == retained);
        QVERIFY(planner.pathForGeometry(oldRef) == retained);
        if (haveOldRoute) {
            QVERIFY(retained);
            QCOMPARE(retained->nodes.size(), std::size_t{3});
            QCOMPARE(retained->geometry.back().x_m, 6.0);
        } else QVERIFY(!retained);
    }
    void invalidRequestsAndMissingGraph() {
        BoostRoutePlanner planner(snapshot(), fixtureProjector, 2.0);
        auto r = request(0); QCOMPARE(planner.requestRoute(r), std::uint64_t{0});
        r = request(1); r.dataVersion = "other"; QCOMPARE(planner.requestRoute(r), std::uint64_t{0});
        r = request(1); r.origin.latitudeDeg = 91; QCOMPARE(planner.requestRoute(r), std::uint64_t{0});
        r = request(1); r.origin.crs = c::CoordinateSystem::GCJ02;
        QCOMPARE(planner.requestRoute(r), std::uint64_t{0});
        r = request(1); r.options = "unsupported"; QCOMPARE(planner.requestRoute(r), std::uint64_t{0});
        BoostRoutePlanner missing({}, fixtureProjector, 2.0);
        QCOMPARE(missing.requestRoute(request(1)), std::uint64_t{0});
        QCOMPARE(planner.requestRoute(request(1)), std::uint64_t{1});
        QCOMPARE(planner.requestRoute(request(1)), std::uint64_t{0}); // Duplicate ID.
        planner.cancel(1);
        finishWorkers();
    }
    void projectionFailureAndException() {
        BoostRoutePlanner planner(snapshot(), fixtureProjector, 2.0);
        QSignalSpy failed(&planner, &c::RoutePlanner::routeFailed);
        auto r = request(1); r.origin.longitudeDeg = 121;
        QCOMPARE(planner.requestRoute(r), std::uint64_t{1});
        QTRY_COMPARE(failed.size(), 1);
        BoostRoutePlanner throwing(snapshot(), [](const auto&) -> std::optional<model::MetricPoint> {
            throw std::runtime_error("test projector failure");
        }, 2.0);
        QSignalSpy exception(&throwing, &c::RoutePlanner::routeFailed);
        QCOMPARE(throwing.requestRoute(request(1)), std::uint64_t{1});
        QTRY_COMPARE(exception.size(), 1);
        QVERIFY(exception.at(0).at(2).toString().contains("test projector failure"));
    }
    void cancellationIsSilentAndGuiKeepsResponding() {
        Gate gate;
        BoostRoutePlanner planner(snapshot(), gate.projector(), 2.0);
        QSignalSpy ready(&planner, &c::RoutePlanner::routeReady);
        QSignalSpy failed(&planner, &c::RoutePlanner::routeFailed);
        QCOMPARE(planner.requestRoute(request(1)), std::uint64_t{1});
        QTRY_VERIFY(gate.state->entered.load());
        bool heartbeat = false;
        QTimer::singleShot(0, this, [&heartbeat] { heartbeat = true; });
        QTRY_VERIFY(heartbeat); // Worker remains blocked while event loop runs.
        QVERIFY(gate.state->offGuiThread.load());
        planner.cancel(1);
        gate.state->release.release(); finishWorkers();
        QCOMPARE(ready.size(), 0); QCOMPARE(failed.size(), 0);
    }
    void latestRequestDiscardsOldResult() {
        Gate gate;
        BoostRoutePlanner planner(snapshot(), gate.projector(), 2.0);
        QSignalSpy ready(&planner, &c::RoutePlanner::routeReady);
        QSignalSpy failed(&planner, &c::RoutePlanner::routeFailed);
        QCOMPARE(planner.requestRoute(request(1)), std::uint64_t{1});
        QTRY_VERIFY(gate.state->entered.load());
        QCOMPARE(planner.requestRoute(request(2)), std::uint64_t{2});
        gate.state->release.release(); finishWorkers();
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(qvariant_cast<c::RoutePlanResult>(ready.at(0).at(0)).requestId, std::uint64_t{2});
        QCOMPARE(failed.size(), 0);
    }
    void destructionWhileWorkerOwnsSnapshot() {
        Gate gate;
        auto* planner = new BoostRoutePlanner(snapshot(), gate.projector(), 2.0);
        int callbacks = 0;
        connect(planner, &c::RoutePlanner::routeReady, this, [&](const auto&) { ++callbacks; });
        QCOMPARE(planner->requestRoute(request(1)), std::uint64_t{1});
        QTRY_VERIFY(gate.state->entered.load());
        delete planner;
        gate.state->release.release(); finishWorkers();
        QCOMPARE(callbacks, 0);
    }
};

QTEST_GUILESS_MAIN(RoutePlannerTests)
#include "RoutePlannerTests.moc"
