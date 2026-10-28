#pragma once
// 固定轨迹回放位置源（4 号，SourceKind::Mock）。只用于本地契约验证、状态机开发和 5 号地图演示；
// 不能作为真实定位(GeoClue2)或 LocationAdapter PAL 适配的验收证据。
// 真实源替换点：在 src/pal/linux/location/ 实现另一个 contracts::LocationSource 子类（第 3 次作业），
// 导航层通过 LocationSource* 使用，无需改动调用方。
//
// 回放模型：
//   - 每个定时步长发出轨迹中的下一帧。墙钟间隔 = stepIntervalMs / speed（至少 1 ms）。
//   - 发出的 timestampMs 取自轨迹文件，不随倍速、暂停改变；回放是确定性的：
//     相同轨迹 + 相同操作序列(start/stepOnce/seekTo/pause/...) → 完全相同的样本序列(含 seq、sessionId)。
//     定时器只决定“何时”发出，不影响“发出什么”。
//   - seq 为本会话发出序号，严格递增；trackFrame 为对应的轨迹帧号。seekTo() 回退时 trackFrame 回退而 seq 继续递增。
//   - 最后一帧发出后：发出 replayFinished(sessionId)，随后自动 stop()（statusChanged(Stopped)）。
//   - 同一次发帧内信号顺序：[statusChanged(Active)，若此前为 NoFix] → positionUpdated → [errorOccurred(AccuracyTooLow)]
//     → [replayFinished → statusChanged(Stopped)]。若消费方在槽中调用 stop()/start()，其后的信号不再发出。
//   - 故障模拟：simulateError() 先改变状态再发出 errorOccurred，供状态机验证权限拒绝、服务不可用、位置丢失与精度过低。
// 线程：与 LocationSource 契约一致，只能在所属线程(GUI 线程)调用；内部 QTimer 与本对象同线程，信号在该线程发出。
#include "contracts/LocationSource.h"
#include "navigation/FixedTrackLoader.h"
#include <QTimer>
#include <cstdint>
#include <optional>

namespace p1::navigation {

class MockLocationSource final : public contracts::LocationSource {
    Q_OBJECT
public:
    static constexpr int kDefaultStepIntervalMs = 1000;
    static constexpr int kMinStepIntervalMs = 10;
    static constexpr int kMaxStepIntervalMs = 60000;
    static constexpr double kMinSpeed = 0.1;
    static constexpr double kMaxSpeed = 32.0;

    explicit MockLocationSource(QObject* parent = nullptr);
    ~MockLocationSource() override;

    // 设置回放轨迹。运行中(Starting/Active/NoFix)拒绝替换并返回 false。
    bool setTrack(FixedTrack track);
    bool hasTrack() const { return track_.has_value(); }
    std::size_t frameCount() const { return track_ ? track_->frames.size() : 0; }

    // LocationSource
    void start() override;
    void stop() override;
    contracts::LocationStatus status() const override { return status_; }
    contracts::SourceKind kind() const override { return contracts::SourceKind::Mock; }
    std::uint64_t sessionId() const override { return sessionId_; }

    // 1 倍速时相邻两帧的墙钟间隔(毫秒)，范围 [10, 60000]；越界返回 false 且不修改。
    bool setStepIntervalMs(int ms);
    int stepIntervalMs() const { return stepIntervalMs_; }
    // 倍速，范围 [0.1, 32]；越界/非有限值返回 false 且不修改。运行中立即生效。
    bool setSpeed(double factor);
    double speed() const { return speed_; }
    int effectiveIntervalMs() const;

    // 暂停/恢复定时回放；仅运行中有效，返回是否改变了状态。暂停不改变 LocationStatus。
    bool pause();
    bool resume();
    bool isPaused() const { return paused_; }

    // 跳转：下一次发出的帧为 trackFrame（0 起，对应 PositionSample::trackFrame）。
    // 仅运行中有效；越界返回 false 且不修改。暂停时跳转不会立即发帧。
    bool seekTo(std::uint64_t trackFrame);
    std::uint64_t nextTrackFrame() const { return cursor_; }
    // 立即发出下一帧（单步，不受暂停影响）；非运行状态返回 false 且不发信号。
    bool stepOnce();

    // 水平精度阈值(米)：帧的 horizontalAccuracyM 大于阈值时，样本照常发出，并追加 AccuracyTooLow 错误。空表示不检查。
    void setMaxHorizontalAccuracyM(std::optional<double> meters) { maxHorizontalAccuracyM_ = meters; }

    // 故障注入（仅运行中有效，返回是否生效）：
    //   PermissionDenied → 状态 PermissionDenied，停止回放；ServiceUnavailable → 状态 Unavailable，停止回放；
    //   PositionLost → 状态 NoFix，下一帧发出时恢复 Active；AccuracyTooLow → 仅发出错误。
    bool simulateError(contracts::LocationErrorCode code, const QString& message = {});

signals:
    void replayFinished(std::uint64_t sessionId);

private:
    bool isRunning() const;
    bool stillCurrent(std::uint64_t session) const { return session == sessionId_ && isRunning(); }
    void setStatus(contracts::LocationStatus status);
    void emitError(contracts::LocationErrorCode code, const QString& message);
    void emitNextFrame();

    QTimer timer_;
    std::optional<FixedTrack> track_;
    contracts::LocationStatus status_ = contracts::LocationStatus::Idle;
    std::uint64_t sessionId_ = 0;
    std::uint64_t nextSeq_ = 0;
    std::uint64_t cursor_ = 0;
    int stepIntervalMs_ = kDefaultStepIntervalMs;
    double speed_ = 1.0;
    bool paused_ = false;
    std::optional<double> maxHorizontalAccuracyM_;
};

} // namespace p1::navigation
