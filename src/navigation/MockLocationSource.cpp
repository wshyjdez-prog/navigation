#include "navigation/MockLocationSource.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace p1::navigation {
namespace {
using contracts::LocationErrorCode;
using contracts::LocationStatus;

std::int64_t wallClockMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
} // namespace

MockLocationSource::MockLocationSource(QObject* parent)
    : contracts::LocationSource(parent), timer_(this) {
    contracts::registerLocationMetaTypes();
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &MockLocationSource::emitNextFrame);
}

// 析构等价于 stop()，但按契约不再发出 statusChanged(Stopped)。
MockLocationSource::~MockLocationSource() {
    timer_.stop();
    status_ = LocationStatus::Stopped;
}

bool MockLocationSource::setTrack(FixedTrack track) {
    if (isRunning() || status_ == LocationStatus::Starting) return false;
    track_ = std::move(track);
    return true;
}

bool MockLocationSource::isRunning() const {
    return status_ == LocationStatus::Active || status_ == LocationStatus::NoFix;
}

void MockLocationSource::start() {
    if (isRunning() || status_ == LocationStatus::Starting) return;
    const std::uint64_t session = ++sessionId_;
    nextSeq_ = 0;
    cursor_ = 0;
    paused_ = false;
    setStatus(LocationStatus::Starting);
    if (session != sessionId_ || status_ != LocationStatus::Starting) return; // 槽中已 stop()/重启
    if (!track_ || track_->frames.empty()) {
        setStatus(LocationStatus::Unavailable);
        if (session == sessionId_ && status_ == LocationStatus::Unavailable)
            emitError(LocationErrorCode::ServiceUnavailable, QStringLiteral("未加载固定轨迹，模拟位置源不可用"));
        return;
    }
    setStatus(LocationStatus::Active);
    if (stillCurrent(session)) timer_.start(effectiveIntervalMs());
}

void MockLocationSource::stop() {
    timer_.stop();
    paused_ = false;
    if (status_ != LocationStatus::Stopped) setStatus(LocationStatus::Stopped);
}

bool MockLocationSource::setStepIntervalMs(int ms) {
    if (ms < kMinStepIntervalMs || ms > kMaxStepIntervalMs) return false;
    stepIntervalMs_ = ms;
    if (timer_.isActive()) timer_.setInterval(effectiveIntervalMs());
    return true;
}

bool MockLocationSource::setSpeed(double factor) {
    if (!std::isfinite(factor) || factor < kMinSpeed || factor > kMaxSpeed) return false;
    speed_ = factor;
    if (timer_.isActive()) timer_.setInterval(effectiveIntervalMs());
    return true;
}

int MockLocationSource::effectiveIntervalMs() const {
    return std::max(1, static_cast<int>(std::lround(stepIntervalMs_ / speed_)));
}

bool MockLocationSource::pause() {
    if (!isRunning() || paused_) return false;
    paused_ = true;
    timer_.stop();
    return true;
}

bool MockLocationSource::resume() {
    if (!isRunning() || !paused_) return false;
    paused_ = false;
    timer_.start(effectiveIntervalMs());
    return true;
}

bool MockLocationSource::seekTo(std::uint64_t trackFrame) {
    if (!isRunning() || trackFrame >= frameCount()) return false;
    cursor_ = trackFrame;
    return true;
}

bool MockLocationSource::stepOnce() {
    if (!isRunning()) return false;
    emitNextFrame();
    return true;
}

bool MockLocationSource::simulateError(LocationErrorCode code, const QString& message) {
    if (!isRunning()) return false;
    const std::uint64_t session = sessionId_;
    const QString text = message.isEmpty()
        ? QStringLiteral("模拟故障：%1").arg(QLatin1String(contracts::toString(code)))
        : message;
    switch (code) {
    case LocationErrorCode::PermissionDenied:
        timer_.stop();
        setStatus(LocationStatus::PermissionDenied);
        break;
    case LocationErrorCode::ServiceUnavailable:
        timer_.stop();
        setStatus(LocationStatus::Unavailable);
        break;
    case LocationErrorCode::PositionLost:
        setStatus(LocationStatus::NoFix);
        break;
    case LocationErrorCode::AccuracyTooLow:
        break;
    }
    // 槽中若已 stop()/重启，则不再发出旧会话的错误。
    if (session == sessionId_ && status_ != LocationStatus::Stopped) emitError(code, text);
    return true;
}

void MockLocationSource::setStatus(LocationStatus status) {
    if (status_ == status) return;
    status_ = status;
    emit statusChanged(status);
}

void MockLocationSource::emitError(LocationErrorCode code, const QString& message) {
    contracts::LocationError error;
    error.code = code;
    error.message = message;
    error.timestampMs = wallClockMs();
    error.sessionId = sessionId_;
    error.source = contracts::SourceKind::Mock;
    emit errorOccurred(error);
}

void MockLocationSource::emitNextFrame() {
    if (!isRunning() || !track_ || cursor_ >= track_->frames.size()) return;
    const std::uint64_t session = sessionId_;
    contracts::PositionSample sample = track_->frames[cursor_];
    sample.source = contracts::SourceKind::Mock;
    sample.seq = nextSeq_++;
    sample.sessionId = session;
    sample.dataVersion = track_->dataVersion;
    sample.trackFrame = cursor_;
    ++cursor_;

    if (status_ == LocationStatus::NoFix) {
        setStatus(LocationStatus::Active);
        if (!stillCurrent(session)) return;
    }
    emit positionUpdated(sample);
    if (!stillCurrent(session)) return;

    if (maxHorizontalAccuracyM_ && sample.horizontalAccuracyM
        && *sample.horizontalAccuracyM > *maxHorizontalAccuracyM_) {
        emitError(LocationErrorCode::AccuracyTooLow,
                  QStringLiteral("第 %1 帧水平精度 %2 米超过阈值 %3 米")
                      .arg(QString::number(*sample.trackFrame),
                           QString::number(*sample.horizontalAccuracyM),
                           QString::number(*maxHorizontalAccuracyM_)));
        if (!stillCurrent(session)) return;
    }

    if (cursor_ >= track_->frames.size()) {
        timer_.stop();
        emit replayFinished(session);
        if (!stillCurrent(session)) return;
        stop();
    }
}

} // namespace p1::navigation
