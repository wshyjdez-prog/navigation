// 4 号位置源契约与固定轨迹回放测试（全部为 Mock 数据，不代表真实 GeoClue2 定位）。
#include "navigation/FixedTrackLoader.h"
#include "navigation/MockLocationSource.h"
#include <QtTest>
#include <cmath>
#include <vector>

using namespace p1::contracts;
using namespace p1::navigation;

namespace {
const QString kFixtureDir = QStringLiteral(P1_LOCATION_FIXTURE_DIR);

bool closeTo(double a, double b) { return std::fabs(a - b) < 1e-9; }
bool closeTo(const std::optional<double>& a, const std::optional<double>& b) {
    return a.has_value() == b.has_value() && (!a || closeTo(*a, *b));
}

bool sameSample(const PositionSample& a, const PositionSample& b) {
    return a.coordinate.crs == b.coordinate.crs && closeTo(a.coordinate.latitudeDeg, b.coordinate.latitudeDeg)
        && closeTo(a.coordinate.longitudeDeg, b.coordinate.longitudeDeg) && closeTo(a.coordinate.altitudeM, b.coordinate.altitudeM)
        && a.timestampMs == b.timestampMs && closeTo(a.speedMps, b.speedMps) && closeTo(a.bearingDeg, b.bearingDeg)
        && closeTo(a.horizontalAccuracyM, b.horizontalAccuracyM) && closeTo(a.verticalAccuracyM, b.verticalAccuracyM)
        && a.source == b.source && a.seq == b.seq && a.sessionId == b.sessionId && a.dataVersion == b.dataVersion
        && a.trackFrame == b.trackFrame;
}

// 收集一个位置源发出的全部信号，顺序与类型分别记录。
struct Recorder {
    std::vector<PositionSample> samples;
    std::vector<LocationStatus> statuses;
    std::vector<LocationError> errors;
    int finished = 0;
    std::size_t total() const { return samples.size() + statuses.size() + errors.size() + static_cast<std::size_t>(finished); }
    void attach(MockLocationSource& source) {
        QObject::connect(&source, &LocationSource::positionUpdated, &source, [this](const PositionSample& s) { samples.push_back(s); });
        QObject::connect(&source, &LocationSource::statusChanged, &source, [this](LocationStatus s) { statuses.push_back(s); });
        QObject::connect(&source, &LocationSource::errorOccurred, &source, [this](const LocationError& e) { errors.push_back(e); });
        QObject::connect(&source, &MockLocationSource::replayFinished, &source, [this](std::uint64_t) { ++finished; });
    }
};

FixedTrack fixtureTrack() {
    auto result = FixedTrackLoader::loadFile(kFixtureDir + QStringLiteral("/campus_loop.csv"));
    return result.ok() ? std::move(*result.track) : FixedTrack{};
}

constexpr char kCsvHeader[] = "timestamp_ms,crs,lat_deg,lon_deg,alt_m,speed_mps,bearing_deg,h_accuracy_m,v_accuracy_m\n";
} // namespace

class LocationReplayTests : public QObject {
    Q_OBJECT
private slots:
    void csvFixtureLoads() {
        const auto result = FixedTrackLoader::loadFile(kFixtureDir + QStringLiteral("/campus_loop.csv"));
        QVERIFY2(result.ok(), qPrintable(result.error));
        const FixedTrack& track = *result.track;
        QCOMPARE(track.frames.size(), std::size_t(12));
        QVERIFY(track.crs == CoordinateSystem::WGS84);
        QCOMPARE(track.dataVersion, QStringLiteral("fixture-campus-loop-v1"));
        QCOMPARE(track.frames.front().timestampMs, std::int64_t(1767225600000));
        QVERIFY(!track.frames[0].bearingDeg.has_value());
        QVERIFY(track.frames[0].coordinate.altitudeM.has_value());
        QVERIFY(!track.frames[3].coordinate.altitudeM.has_value());
        QVERIFY(!track.frames[3].verticalAccuracyM.has_value());
        QVERIFY(closeTo(*track.frames[6].bearingDeg, 90.0));
        for (const auto& frame : track.frames) QVERIFY(frame.source == SourceKind::Mock);
    }

    void jsonMatchesCsv() {
        const auto csv = FixedTrackLoader::loadFile(kFixtureDir + QStringLiteral("/campus_loop.csv"));
        const auto json = FixedTrackLoader::loadFile(kFixtureDir + QStringLiteral("/campus_loop.json"));
        QVERIFY2(csv.ok(), qPrintable(csv.error));
        QVERIFY2(json.ok(), qPrintable(json.error));
        QCOMPARE(json.track->dataVersion, csv.track->dataVersion);
        QCOMPARE(json.track->frames.size(), csv.track->frames.size());
        for (std::size_t i = 0; i < csv.track->frames.size(); ++i)
            QVERIFY2(sameSample(csv.track->frames[i], json.track->frames[i]), qPrintable(QString::number(i)));
    }

    void csvErrorsAreExplicit_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("expected");
        const QString h = QString::fromLatin1(kCsvHeader);
        const QString v = QStringLiteral("# dataVersion=t1\n");
        QTest::newRow("missing-version") << h + QStringLiteral("1,WGS84,1,1,,,,,\n") << QStringLiteral("dataVersion");
        QTest::newRow("missing-crs-column") << v + QStringLiteral("timestamp_ms,lat_deg,lon_deg\n1,1,1\n") << QStringLiteral("缺少必需列 crs");
        QTest::newRow("unknown-crs") << v + h + QStringLiteral("1,WGS-84,1,1,,,,,\n") << QStringLiteral("t.csv:3:");
        QTest::newRow("unknown-column") << v + QStringLiteral("timestamp_ms,crs,lat_deg,lon_deg,speed_kmh\n") << QStringLiteral("speed_kmh");
        QTest::newRow("lat-range") << v + h + QStringLiteral("1,WGS84,91,1,,,,,\n") << QStringLiteral("t.csv:3: 纬度");
        QTest::newRow("bad-number") << v + h + QStringLiteral("1,WGS84,1,abc,,,,,\n") << QStringLiteral("lon_deg");
        QTest::newRow("field-count") << v + h + QStringLiteral("1,WGS84,1,1\n") << QStringLiteral("列数");
        QTest::newRow("time-order") << v + h + QStringLiteral("2,WGS84,1,1,,,,,\n2,WGS84,1,1,,,,,\n") << QStringLiteral("t.csv:4: 时间戳");
        QTest::newRow("mixed-crs") << v + h + QStringLiteral("1,WGS84,1,1,,,,,\n2,GCJ02,1,1,,,,,\n") << QStringLiteral("不一致");
        QTest::newRow("bearing-range") << v + h + QStringLiteral("1,WGS84,1,1,,,360,,\n") << QStringLiteral("方向");
        QTest::newRow("no-frames") << v + h << QStringLiteral("没有任何轨迹帧");
    }
    void csvErrorsAreExplicit() {
        QFETCH(QString, text);
        QFETCH(QString, expected);
        const auto result = FixedTrackLoader::parseCsv(text, QStringLiteral("t.csv"));
        QVERIFY(!result.ok());
        QVERIFY2(result.error.contains(expected), qPrintable(result.error));
    }

    void jsonAndFileErrorsAreExplicit() {
        auto r = FixedTrackLoader::parseJson("{ broken", QStringLiteral("t.json"));
        QVERIFY(!r.ok() && r.error.contains(QStringLiteral("JSON 解析失败")));
        r = FixedTrackLoader::parseJson(R"({"format":"p1-fixed-track","formatVersion":1,"dataVersion":"x","frames":[]})",
                                        QStringLiteral("t.json"));
        QVERIFY2(!r.ok() && r.error.contains(QStringLiteral("crs")), qPrintable(r.error));
        r = FixedTrackLoader::parseJson(R"({"format":"p1-fixed-track","formatVersion":1,"dataVersion":"x","crs":"WGS84",
            "frames":[{"timestampMs":1.5,"latDeg":1,"lonDeg":1}]})", QStringLiteral("t.json"));
        QVERIFY2(!r.ok() && r.error.contains(QStringLiteral("frames[0]")), qPrintable(r.error));
        r = FixedTrackLoader::loadFile(kFixtureDir + QStringLiteral("/does-not-exist.csv"));
        QVERIFY2(!r.ok() && r.error.contains(QStringLiteral("无法打开")), qPrintable(r.error));
        r = FixedTrackLoader::loadFile(kFixtureDir + QStringLiteral("/campus_loop.gpx"));
        QVERIFY2(!r.ok() && r.error.contains(QStringLiteral("不支持")), qPrintable(r.error));
    }

    void startWithoutTrackIsUnavailable() {
        MockLocationSource source;
        Recorder rec;
        rec.attach(source);
        source.start();
        QVERIFY(source.status() == LocationStatus::Unavailable);
        QVERIFY((rec.statuses == std::vector<LocationStatus>{LocationStatus::Starting, LocationStatus::Unavailable}));
        QCOMPARE(rec.errors.size(), std::size_t(1));
        QVERIFY(rec.errors[0].code == LocationErrorCode::ServiceUnavailable);
        QVERIFY(rec.samples.empty());
    }

    void replayIsDeterministicAndSeqMonotonic() {
        std::vector<PositionSample> runs[2];
        for (auto& run : runs) {
            MockLocationSource source;
            Recorder rec;
            rec.attach(source);
            QVERIFY(source.setTrack(fixtureTrack()));
            source.start();
            for (int i = 0; i < 3; ++i) QVERIFY(source.stepOnce());
            QVERIFY(source.seekTo(1));
            QVERIFY(!source.seekTo(12)); // 越界不改变游标
            QVERIFY(source.stepOnce());
            QVERIFY(source.stepOnce());
            run = rec.samples;
        }
        QCOMPARE(runs[0].size(), std::size_t(5));
        QCOMPARE(runs[1].size(), runs[0].size());
        const std::uint64_t expectedFrames[] = {0, 1, 2, 1, 2};
        for (std::size_t i = 0; i < runs[0].size(); ++i) {
            QVERIFY(sameSample(runs[0][i], runs[1][i]));
            QCOMPARE(runs[0][i].seq, std::uint64_t(i));
            QCOMPARE(*runs[0][i].trackFrame, expectedFrames[i]);
            QCOMPARE(runs[0][i].sessionId, std::uint64_t(1));
            QVERIFY(runs[0][i].source == SourceKind::Mock);
            QCOMPARE(runs[0][i].dataVersion, QStringLiteral("fixture-campus-loop-v1"));
        }
        QCOMPARE(runs[0][3].timestampMs, runs[0][1].timestampMs); // 回退后时间戳来自轨迹，不是墙钟
    }

    void timerPauseResumeAndSpeed() {
        MockLocationSource source;
        Recorder rec;
        rec.attach(source);
        QVERIFY(source.setTrack(fixtureTrack()));
        QVERIFY(source.setStepIntervalMs(40));
        QVERIFY(source.setSpeed(2.0));
        QCOMPARE(source.effectiveIntervalMs(), 20);
        QVERIFY(!source.setSpeed(0.0));
        QVERIFY(!source.setSpeed(64.0));
        QVERIFY(!source.setStepIntervalMs(5));
        QCOMPARE(source.effectiveIntervalMs(), 20);
        QVERIFY(!source.pause()); // 未运行时无效
        source.start();
        QTRY_VERIFY(rec.samples.size() >= 2);
        QVERIFY(source.pause());
        QVERIFY(source.isPaused());
        QVERIFY(source.status() == LocationStatus::Active);
        const std::size_t frozen = rec.samples.size();
        QTest::qWait(150);
        QCOMPARE(rec.samples.size(), frozen);
        QVERIFY(source.stepOnce()); // 暂停时允许单步
        QCOMPARE(rec.samples.size(), frozen + 1);
        QVERIFY(source.setSpeed(4.0));
        QCOMPARE(source.effectiveIntervalMs(), 10);
        QVERIFY(source.resume());
        QTRY_VERIFY(rec.samples.size() >= frozen + 3);
        for (std::size_t i = 1; i < rec.samples.size(); ++i) QVERIFY(rec.samples[i].seq > rec.samples[i - 1].seq);
    }

    void noCallbacksAfterStop() {
        MockLocationSource source;
        Recorder rec;
        rec.attach(source);
        QVERIFY(source.setTrack(fixtureTrack()));
        QVERIFY(source.setStepIntervalMs(10));
        source.start();
        QTRY_VERIFY(!rec.samples.empty());
        source.stop();
        QVERIFY(rec.statuses.back() == LocationStatus::Stopped);
        const std::size_t total = rec.total();
        QTest::qWait(100);
        QVERIFY(!source.stepOnce());
        QVERIFY(!source.seekTo(0));
        QVERIFY(!source.resume());
        QVERIFY(!source.simulateError(LocationErrorCode::PositionLost));
        source.stop(); // 重复 stop() 为空操作
        QCOMPARE(rec.total(), total);
        QVERIFY(source.status() == LocationStatus::Stopped);
    }

    void stopInsideSlotSuppressesRemainingSignals() {
        MockLocationSource source;
        Recorder rec;
        rec.attach(source);
        QVERIFY(source.setTrack(fixtureTrack()));
        source.setMaxHorizontalAccuracyM(1.0); // 每帧都会触发 AccuracyTooLow
        QObject::connect(&source, &LocationSource::positionUpdated, &source, [&source] { source.stop(); });
        source.start();
        QVERIFY(source.stepOnce());
        QCOMPARE(rec.samples.size(), std::size_t(1));
        QVERIFY(rec.errors.empty()); // 槽中 stop() 后，同一发帧内的精度错误不再发出
        QVERIFY(rec.statuses.back() == LocationStatus::Stopped);
    }

    void replayEndsWithFinishedThenStopped() {
        MockLocationSource source;
        Recorder rec;
        rec.attach(source);
        QVERIFY(source.setTrack(fixtureTrack()));
        source.start();
        QVERIFY(!source.setTrack(fixtureTrack())); // 运行中不可替换
        for (int i = 0; i < 12; ++i) QVERIFY(source.stepOnce());
        QCOMPARE(rec.samples.size(), std::size_t(12));
        QCOMPARE(rec.finished, 1);
        QVERIFY(source.status() == LocationStatus::Stopped);
        QVERIFY(!source.stepOnce());
        // 重新 start() 开启新会话：sessionId 递增、seq 与帧号从 0 开始
        source.start();
        QVERIFY(source.stepOnce());
        QCOMPARE(rec.samples.back().sessionId, std::uint64_t(2));
        QCOMPARE(rec.samples.back().seq, std::uint64_t(0));
        QCOMPARE(*rec.samples.back().trackFrame, std::uint64_t(0));
    }

    void simulatedErrorsMapToStatus() {
        MockLocationSource source;
        Recorder rec;
        rec.attach(source);
        QVERIFY(source.setTrack(fixtureTrack()));
        source.start();
        QVERIFY(source.simulateError(LocationErrorCode::PositionLost));
        QVERIFY(source.status() == LocationStatus::NoFix);
        QVERIFY(rec.errors.back().code == LocationErrorCode::PositionLost);
        QVERIFY(source.stepOnce()); // 下一帧恢复 Active
        QVERIFY(source.status() == LocationStatus::Active);

        source.setMaxHorizontalAccuracyM(20.0); // 第 9 帧 25 m 超阈值
        const std::size_t errorsBefore = rec.errors.size();
        while (source.nextTrackFrame() <= 9) QVERIFY(source.stepOnce());
        QCOMPARE(rec.errors.size(), errorsBefore + 1);
        QVERIFY(rec.errors.back().code == LocationErrorCode::AccuracyTooLow);
        QCOMPARE(*rec.samples.back().trackFrame, std::uint64_t(9)); // 样本照常发出

        QVERIFY(source.simulateError(LocationErrorCode::PermissionDenied));
        QVERIFY(source.status() == LocationStatus::PermissionDenied);
        QVERIFY(rec.errors.back().code == LocationErrorCode::PermissionDenied);
        QVERIFY(!source.stepOnce());
        source.start(); // 授权恢复后可重试
        QVERIFY(source.status() == LocationStatus::Active);
        QVERIFY(source.simulateError(LocationErrorCode::ServiceUnavailable));
        QVERIFY(source.status() == LocationStatus::Unavailable);
        QCOMPARE(rec.errors.back().sessionId, std::uint64_t(2));
    }
};

QTEST_GUILESS_MAIN(LocationReplayTests)
#include "LocationReplayTests.moc"
