#include "ui/map/NavigationSimulation.h"

#include <QPointF>
#include <QVector>
#include <QtTest>

using namespace p1;

class NavigationSimulationTests : public QObject {
    Q_OBJECT
private:
    static QVector<QPointF> route() {
        return {{114.3000, 30.5900}, {114.3010, 30.5900}, {114.3010, 30.5910}};
    }

private slots:
    void routeProgressPauseAndResume() {
        ui::NavigationSimulation simulation;
        QVERIFY(simulation.setRoute(route(), 10.0));
        QVERIFY(simulation.hasRoute());
        simulation.start();
        QVERIFY(simulation.running());
        QVERIFY(simulation.snapshot());
        QCOMPARE(simulation.snapshot()->source, contracts::SourceKind::Mock);
        QCOMPARE(simulation.snapshot()->guidance->kind, contracts::GuidanceKind::TurnLeft);

        const double initialRemaining = simulation.remainingM();
        simulation.advance(1000);
        QVERIFY(simulation.travelledM() > 9.0);
        QVERIFY(simulation.remainingM() < initialRemaining);

        simulation.pause();
        QVERIFY(simulation.paused());
        QVERIFY(simulation.snapshot()->paused);
        const double pausedTravelled = simulation.travelledM();
        simulation.advance(5000);
        QCOMPARE(simulation.travelledM(), pausedTravelled);

        simulation.resume();
        QVERIFY(!simulation.paused());
        simulation.advance(1000);
        QVERIFY(simulation.travelledM() > pausedTravelled);
    }

    void playbackMultiplierAcceleratesProgress() {
        ui::NavigationSimulation normal;
        ui::NavigationSimulation fast;
        QVERIFY(normal.setRoute(route(), 10.0));
        QVERIFY(fast.setRoute(route(), 10.0));
        fast.setSpeedMultiplier(4.0);
        normal.start();
        fast.start();
        normal.advance(1000);
        fast.advance(1000);
        QVERIFY(qAbs(fast.travelledM() - normal.travelledM() * 4.0) < 0.01);
        QCOMPARE(fast.speedMultiplier(), 4.0);
    }

    void geodesicRouteLengthUsesBoostGeometry() {
        ui::NavigationSimulation simulation;
        QVERIFY(simulation.setRoute(route(), 10.0));
        const double total = simulation.remainingM();
        // Vincenty on the WGS84 spheroid: about 95.8 m east + 111.1 m north.
        QVERIFY2(total > 205.0 && total < 209.0,
                 qPrintable(QString::number(total, 'f', 3)));
    }

    void arrivalProducesTerminalGuidance() {
        ui::NavigationSimulation simulation;
        QVERIFY(simulation.setRoute(route(), 10.0));
        simulation.start();
        simulation.advance(60000);
        QVERIFY(simulation.arrived());
        QVERIFY(!simulation.running());
        QCOMPARE(simulation.remainingM(), 0.0);
        QCOMPARE(simulation.snapshot()->status, contracts::NavigationStatus::Arrived);
        QCOMPARE(simulation.snapshot()->guidance->kind, contracts::GuidanceKind::ArriveDestination);
        QCOMPARE(simulation.snapshot()->currentPosition->speedMps.value_or(-1.0), 0.0);
    }

    void invalidRoutesAreRejected() {
        ui::NavigationSimulation simulation;
        QVERIFY(!simulation.setRoute({{114.3, 30.59}}, 10.0));
        QVERIFY(!simulation.setRoute(route(), 0.0));
        QVERIFY(!simulation.setRoute({{114.3, 30.59}, {181.0, 30.59}}, 10.0));
        QVERIFY(!simulation.hasRoute());
        simulation.start();
        QVERIFY(!simulation.running());
        QVERIFY(!simulation.snapshot());
    }
};

QTEST_GUILESS_MAIN(NavigationSimulationTests)
#include "NavigationSimulationTests.moc"
