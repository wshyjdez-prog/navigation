// 4 号五类模拟样例的加载与回放测试（Stage 3 样例 + Stage 4 回放 + Stage 5 契约测试）。
// 全部为 Mock 数据：虚构网格坐标，不接真实定位，不作 PAL 适配或 G6 联调证据。
// 期望值取自 tests/fixtures/location/expected/*.md，均为可手算复核的数值。
#include "navigation/FixedTrackLoader.h"
#include "navigation/MockLocationSource.h"

#include <QtTest>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace p1::contracts;
using namespace p1::navigation;

namespace {
const QString kFixtureDir = QStringLiteral(P1_LOCATION_FIXTURE_DIR);

// 构造常量（与 tests/fixtures/location/README.md 一致）
constexpr double kMetersPerLatDegree = 111320.0;
constexpr double kMetersPerLonDegreeAtLat60 = 55660.0;
constexpr double kOriginLat = 60.0;
constexpr double kOriginLon = 116.0;
constexpr double kRouteLengthM = 190.3572; // 19 段 × 10.0188 米
constexpr double kOffRouteThresholdM = 15.0; // 第 ⑥ 节已确认：偏离 > 15 米
constexpr int kOffRouteSustainedFrames = 3;  // 第 ⑥ 节已确认：持续 ≥ 3 秒（时间步 1000 毫秒）

FixedTrack load(const QString& name) {
    const auto result = FixedTrackLoader::loadFile(kFixtureDir + QLatin1Char('/') + name);
    if (!result.ok()) qFatal("%s", qPrintable(result.error));
    return *result.track;
}

double progressM(const PositionSample& sample) {
    return (sample.coordinate.latitudeDeg - kOriginLat) * kMetersPerLatDegree;
}
double lateralDeviationM(const PositionSample& sample) {
    return std::fabs(sample.coordinate.longitudeDeg - kOriginLon) * kMetersPerLonDegreeAtLat60;
}

bool near(double a, double b) { return std::fabs(a - b) < 0.01; }

// 回放整条轨迹，返回收到的样本（单步驱动，确定性，与倍速/墙钟无关）。
struct ReplayResult {
    std::vector<PositionSample> samples;
    std::vector<LocationError> errors;
    LocationStatus finalStatus = LocationStatus::Idle;
    int finished = 0;
};

// 单步回放整条轨迹并把结果写入 out（用 out 参数是因为 QVERIFY 只能在返回 void 的函数里使用）。
void runReplay(const FixedTrack& track, std::optional<double> maxAccuracyM, ReplayResult& out) {
    MockLocationSource source;
    if (maxAccuracyM) source.setMaxHorizontalAccuracyM(*maxAccuracyM);
    QObject::connect(&source, &LocationSource::positionUpdated, &source,
                     [&out](const PositionSample& s) { out.samples.push_back(s); });
    QObject::connect(&source, &LocationSource::errorOccurred, &source,
                     [&out](const LocationError& e) { out.errors.push_back(e); });
    QObject::connect(&source, &MockLocationSource::replayFinished, &source, [&out](std::uint64_t) { ++out.finished; });
    QVERIFY2(source.setTrack(track), "setTrack failed");
    source.start();
    for (std::size_t i = 0; i < source.frameCount(); ++i) QVERIFY(source.stepOnce());
    out.finalStatus = source.status();
}
} // namespace

class LocationFixtureTests : public QObject {
    Q_OBJECT
private slots:
    // 覆盖 Stage 3 验收：五类样例字段齐全、坐标系一致、时间严格递增、全部 source=mock。
    void allFiveScenariosLoadWithDeclaredShape() {
        struct Expectation { const char* file; std::size_t frames; const char* version; };
        const Expectation expected[] = {
            {"normal_straight.csv", 10, "fixture-normal-straight-v1"},
            {"off_route_recover.csv", 14, "fixture-off-route-recover-v1"},
            {"arrive_destination.csv", 20, "fixture-arrive-destination-v1"},
            {"degraded_signal.csv", 10, "fixture-degraded-signal-v1"},
            {"jump_backward.csv", 10, "fixture-jump-backward-v1"},
        };
        for (const auto& expectation : expected) {
            const FixedTrack track = load(QString::fromLatin1(expectation.file));
            QCOMPARE(track.frames.size(), expectation.frames);
            QCOMPARE(track.dataVersion, QString::fromLatin1(expectation.version));
            QVERIFY2(track.crs == CoordinateSystem::WGS84, expectation.file);
            QCOMPARE(track.frames.front().timestampMs, std::int64_t(1767225600000)); // 2026-01-01T00:00:00Z
            for (std::size_t i = 0; i < track.frames.size(); ++i) {
                const PositionSample& frame = track.frames[i];
                QVERIFY2(frame.source == SourceKind::Mock, expectation.file);
                QVERIFY2(frame.timestampMs > 0, expectation.file);
                if (i > 0)
                    QVERIFY2(frame.timestampMs > track.frames[i - 1].timestampMs, "时间戳必须严格递增");
                QVERIFY2(frame.coordinate.crs == CoordinateSystem::WGS84, expectation.file);
                QVERIFY2(!frame.horizontalAccuracyM || *frame.horizontalAccuracyM >= 0.0, expectation.file);
            }
        }
    }

    // 覆盖 Stage 3 场景 A：进度与剩余距离可手算复核（第 9 帧 90.1692 米 / 剩余 100.1880 米）。
    void straightScenarioMatchesHandCalculation() {
        const FixedTrack track = load(QStringLiteral("normal_straight.csv"));
        for (std::size_t i = 0; i < track.frames.size(); ++i) {
            QVERIFY2(near(progressM(track.frames[i]), 10.0188 * double(i)), qPrintable(QString::number(i)));
            QVERIFY2(near(lateralDeviationM(track.frames[i]), 0.0), "正常直行横向偏离必须为 0");
        }
        const PositionSample& last = track.frames.back();
        QVERIFY(near(progressM(last), 90.1692));
        QVERIFY(near(kRouteLengthM - progressM(last), 100.1880));
        QVERIFY(near(*last.speedMps, 10.02));
        QVERIFY(near(*last.bearingDeg, 0.0)); // 正北
    }

    // 覆盖 Stage 3 场景 B + 第 ⑥ 节：偏离峰值 20.0376 米，且连续 3 帧超过 15 米阈值（持续 3 秒）。
    void offRouteScenarioSustainsDeviationForThreeSeconds() {
        const FixedTrack track = load(QStringLiteral("off_route_recover.csv"));
        double peak = 0.0;
        int sustained = 0;
        int maxSustained = 0;
        for (const auto& frame : track.frames) {
            const double deviation = lateralDeviationM(frame);
            peak = std::max(peak, deviation);
            if (deviation > kOffRouteThresholdM) {
                ++sustained;
                maxSustained = std::max(maxSustained, sustained);
            } else {
                sustained = 0;
            }
        }
        QVERIFY(near(peak, 20.0376));
        QCOMPARE(maxSustained, kOffRouteSustainedFrames);
        // 回归：最后一帧回到中心线
        QVERIFY(near(lateralDeviationM(track.frames.back()), 0.0));
    }

    // 覆盖 Stage 3 场景 C：第 19 帧进度等于路线总长 190.3572 米，剩余 0。
    void arrivalScenarioEndsAtTotalDistance() {
        const FixedTrack track = load(QStringLiteral("arrive_destination.csv"));
        QCOMPARE(track.frames.size(), std::size_t(20));
        for (std::size_t i = 0; i < track.frames.size(); ++i)
            QVERIFY2(near(progressM(track.frames[i]), 10.0188 * double(i)), qPrintable(QString::number(i)));
        QVERIFY(near(progressM(track.frames.back()), kRouteLengthM));
        QVERIFY(near(kRouteLengthM - progressM(track.frames.back()), 0.0));
        QVERIFY(near(*track.frames.back().speedMps, 0.0)); // 到达后速度 0
    }

    // 覆盖 Stage 3 场景 D：低速静止（连续 3 帧位移 0）、30 秒时间空档、后 3 帧水平精度 35 米。
    void degradedScenarioHasGapZeroDisplacementAndLowAccuracy() {
        const FixedTrack track = load(QStringLiteral("degraded_signal.csv"));
        std::int64_t maxGap = 0;
        int zeroDisplacement = 0;
        int lowAccuracy = 0;
        for (std::size_t i = 0; i < track.frames.size(); ++i) {
            if (i > 0) {
                const std::int64_t gap = track.frames[i].timestampMs - track.frames[i - 1].timestampMs;
                maxGap = std::max(maxGap, gap);
                if (near(progressM(track.frames[i]) - progressM(track.frames[i - 1]), 0.0)) ++zeroDisplacement;
            }
            if (track.frames[i].horizontalAccuracyM && *track.frames[i].horizontalAccuracyM > 20.0) ++lowAccuracy;
        }
        QCOMPARE(maxGap, std::int64_t(30000)); // 位置丢失：30 秒没有任何样本
        QCOMPARE(zeroDisplacement, 3);         // 帧 4-6 静止
        QCOMPARE(lowAccuracy, 3);              // 帧 7-9 精度 35 米
        QVERIFY(near(*track.frames[4].speedMps, 0.0));
    }

    // 覆盖 Stage 3 场景 E：时间合法但位置倒退（恰好 1 帧，幅度 −20.0376 米）。
    void backwardJumpIsDetected() {
        const FixedTrack track = load(QStringLiteral("jump_backward.csv"));
        int backward = 0;
        double worstDelta = 0.0;
        for (std::size_t i = 1; i < track.frames.size(); ++i) {
            const double delta = progressM(track.frames[i]) - progressM(track.frames[i - 1]);
            if (delta < 0.0) {
                ++backward;
                worstDelta = std::min(worstDelta, delta);
            }
        }
        QCOMPARE(backward, 1);
        QVERIFY(near(worstDelta, -20.0376));
    }

    // 覆盖 Stage 4 验收：五类样例均可回放，seq 严格递增、trackFrame 对应、source=mock，
    // 回放结束自动转 Stopped；精度过低帧触发 AccuracyTooLow 但样本照常发出。
    void replayMatchesExpectedSequence() {
        const char* files[] = {"normal_straight.csv", "off_route_recover.csv", "arrive_destination.csv",
                               "degraded_signal.csv", "jump_backward.csv"};
        const std::size_t frames[] = {10, 14, 20, 10, 10};
        for (int i = 0; i < 5; ++i) {
            const FixedTrack track = load(QString::fromLatin1(files[i]));
            ReplayResult result;
            runReplay(track, 20.0, result); // 精度阈值 20 米
            QCOMPARE(result.samples.size(), frames[i]);
            QVERIFY2(result.finalStatus == LocationStatus::Stopped, files[i]);
            QCOMPARE(result.finished, 1);
            for (std::size_t k = 0; k < result.samples.size(); ++k) {
                QCOMPARE(result.samples[k].seq, std::uint64_t(k));
                QCOMPARE(*result.samples[k].trackFrame, std::uint64_t(k));
                QVERIFY(result.samples[k].source == SourceKind::Mock);
                QCOMPARE(result.samples[k].sessionId, std::uint64_t(1));
                QCOMPARE(result.samples[k].timestampMs, track.frames[k].timestampMs);
            }
        }
        // 场景 D 的三帧低精度：样本照发 + 3 次 AccuracyTooLow
        ReplayResult degraded;
        runReplay(load(QStringLiteral("degraded_signal.csv")), 20.0, degraded);
        QCOMPARE(degraded.errors.size(), std::size_t(3));
        for (const auto& error : degraded.errors) QVERIFY(error.code == LocationErrorCode::AccuracyTooLow);
        QCOMPARE(degraded.samples.size(), std::size_t(10)); // 样本未被丢弃
    }
};

QTEST_GUILESS_MAIN(LocationFixtureTests)
#include "LocationFixtureTests.moc"
