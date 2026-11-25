#pragma once

#include "contracts/NavigationTypes.h"

#include <QPointF>
#include <QVector>

#include <optional>

namespace p1::ui {

// UI-side deterministic playback used for the third-assignment demonstration.
// It produces the same NavigationSnapshot consumed by production presentation
// code; it is not a navigation state machine and never marks source=real.
class NavigationSimulation final {
public:
    bool setRoute(const QVector<QPointF>& geoRoute,
                  double speedMps = 8.33,
                  const QString& dataVersion = QStringLiteral("mock-navigation-demo"));
    void start();
    void pause();
    void resume();
    void stop();
    void setSpeedMultiplier(double multiplier);
    void advance(int elapsedMs);

    bool hasRoute() const { return cumulativeM_.size() >= 2; }
    bool running() const { return running_; }
    bool paused() const { return paused_; }
    bool arrived() const { return arrived_; }
    double speedMultiplier() const { return speedMultiplier_; }
    double travelledM() const { return travelledM_; }
    double remainingM() const { return qMax(0.0, totalM_ - travelledM_); }
    const std::optional<contracts::NavigationSnapshot>& snapshot() const { return snapshot_; }

private:
    void updateSnapshot();
    QPointF pointAt(double distanceM, double* bearingDeg) const;
    void updateGuidance();

    QVector<QPointF> geoRoute_;
    QVector<double> cumulativeM_;
    QString dataVersion_;
    std::optional<contracts::NavigationSnapshot> snapshot_;
    double baseSpeedMps_ = 8.33;
    double speedMultiplier_ = 1.0;
    double totalM_ = 0.0;
    double travelledM_ = 0.0;
    std::uint64_t navigationId_ = 0;
    bool running_ = false;
    bool paused_ = false;
    bool arrived_ = false;
};

} // namespace p1::ui
