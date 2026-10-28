// 最小演示（4 号，Mock 数据）：加载固定轨迹 → start → 逐帧输出 PositionSample →
// pause（确认暂停期间无样本）→ seekTo 回退 → resume → setSpeed → stop（确认之后无回调）。
// 用法：p1-location-demo [轨迹.csv|轨迹.json]；全部检查通过返回 0，失败返回 1，加载失败返回 2，超时返回 3。
// 输出均为模拟位置（src=mock），不代表真实 GeoClue2 定位。
#include "navigation/FixedTrackLoader.h"
#include "navigation/MockLocationSource.h"
#include <QCoreApplication>
#include <QTextStream>
#include <QTimer>
#include <optional>
#include <utility>
#include <vector>

using namespace p1::contracts;
using p1::navigation::FixedTrackLoader;
using p1::navigation::MockLocationSource;

namespace {
QString optionalText(const std::optional<double>& value) {
    return value ? QString::number(*value) : QStringLiteral("-");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    const QString path = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                  : QStringLiteral(P1_LOCATION_FIXTURE_DIR "/campus_loop.csv");

    auto loaded = FixedTrackLoader::loadFile(path);
    if (!loaded.ok()) {
        QTextStream(stderr) << "[error] " << loaded.error << Qt::endl;
        return 2;
    }
    out << "[load] " << path << " frames=" << loaded.track->frames.size()
        << " crs=" << toString(loaded.track->crs) << " dataVersion=" << loaded.track->dataVersion << Qt::endl;

    MockLocationSource source;
    source.setStepIntervalMs(100);
    source.setSpeed(2.0); // 50 ms/帧
    source.setTrack(std::move(*loaded.track));

    int failures = 0;
    bool stopped = false;
    int callbacksAfterStop = 0;
    std::optional<std::uint64_t> lastSeq;
    bool seqMonotonic = true;
    std::vector<std::uint64_t> frames;
    const auto check = [&](bool ok, const QString& what) {
        out << (ok ? "[PASS] " : "[FAIL] ") << what << Qt::endl;
        if (!ok) ++failures;
    };

    QObject::connect(&source, &LocationSource::statusChanged, &app, [&](LocationStatus status) {
        if (stopped) ++callbacksAfterStop;
        out << "[status] " << toString(status) << Qt::endl;
    });
    QObject::connect(&source, &LocationSource::errorOccurred, &app, [&](const LocationError& error) {
        if (stopped) ++callbacksAfterStop;
        out << "[error] " << toString(error.code) << " session=" << error.sessionId << " " << error.message << Qt::endl;
    });
    QObject::connect(&source, &LocationSource::positionUpdated, &app, [&](const PositionSample& s) {
        if (stopped) {
            ++callbacksAfterStop;
            return;
        }
        seqMonotonic = seqMonotonic && (!lastSeq || s.seq > *lastSeq);
        lastSeq = s.seq;
        frames.push_back(*s.trackFrame);
        out << "[sample] session=" << s.sessionId << " seq=" << s.seq << " frame=" << *s.trackFrame
            << " t=" << s.timestampMs << " lat=" << QString::number(s.coordinate.latitudeDeg, 'f', 6)
            << " lon=" << QString::number(s.coordinate.longitudeDeg, 'f', 6)
            << " crs=" << toString(s.coordinate.crs) << " alt=" << optionalText(s.coordinate.altitudeM)
            << " v=" << optionalText(s.speedMps) << " bearing=" << optionalText(s.bearingDeg)
            << " hacc=" << optionalText(s.horizontalAccuracyM) << " src=" << toString(s.source)
            << " ver=" << s.dataVersion << Qt::endl;

        if (s.seq == 2) {
            check(source.pause(), QStringLiteral("pause()"));
            const std::size_t frozen = frames.size();
            QTimer::singleShot(300, &app, [&, frozen] {
                check(frames.size() == frozen, QStringLiteral("暂停 300 ms 内没有新样本"));
                check(source.seekTo(1), QStringLiteral("seekTo(1)：下一帧回到轨迹第 1 帧"));
                check(source.resume(), QStringLiteral("resume()"));
            });
        } else if (s.seq == 5) {
            const bool changed = source.setSpeed(4.0);
            check(changed && source.effectiveIntervalMs() == 25,
                  QStringLiteral("setSpeed(4.0)，间隔 %1 ms").arg(source.effectiveIntervalMs()));
        } else if (s.seq == 8) {
            source.stop(); // 同步发出 statusChanged(Stopped)，返回后不得再有回调
            stopped = true;
            QTimer::singleShot(400, &app, [&] {
                check(callbacksAfterStop == 0, QStringLiteral("stop() 后 400 ms 内回调次数 = %1").arg(callbacksAfterStop));
                check(!source.stepOnce(), QStringLiteral("stop() 后 stepOnce() 被拒绝且不发信号"));
                check(seqMonotonic, QStringLiteral("seq 严格递增（含 seek 回退）"));
                const std::vector<std::uint64_t> expected{0, 1, 2, 1, 2, 3, 4, 5, 6};
                check(frames == expected, QStringLiteral("帧序列为 0,1,2,[seek]1,2,3,4,5,6"));
                check(source.status() == LocationStatus::Stopped, QStringLiteral("最终状态 Stopped"));
                out << (failures == 0 ? "[result] 全部通过" : "[result] 存在失败") << Qt::endl;
                app.exit(failures == 0 ? 0 : 1);
            });
        }
    });

    QTimer::singleShot(10000, &app, [&] {
        out << "[result] 超时" << Qt::endl;
        app.exit(3);
    });
    source.start();
    return app.exec();
}
