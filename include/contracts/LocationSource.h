#pragma once
// 位置源抽象契约（4 号设计，1 号维护 include/contracts/）。
// 实现：
//   - p1::navigation::MockLocationSource（src/navigation/，固定轨迹回放，SourceKind::Mock）
//   - 替换点：GeoClue2 真实源（规划 src/pal/linux/location/，第 3 次作业接入，SourceKind::Real）
// 导航状态机只依赖本接口，不依赖具体实现，故真实源与模拟源可以互换。
//
// ① 线程归属
//   - LocationSource 是 QObject，所有方法(start/stop/status 及实现类的控制方法)只能在其所属线程调用；
//     正式工程中该线程就是 GUI 主线程。
//   - 信号在源对象所属线程发出。消费方若在 GUI 线程则为直接调用；位于其他线程的实现
//     (例如在 D-Bus 工作线程收到 GeoClue2 回调) 必须把数据经 Qt::QueuedConnection 投递到 GUI 线程
//     再 emit，不得在后台线程直接触碰任何 QWidget/UI 对象。
//   - 信号参数按值复制(const& 传入，排队连接时由 Qt 复制)，不引用源内部存储。
//
// ② 所有权与生命周期
//   - 源对象由创建者通过 QObject 父子关系或 std::unique_ptr 独占持有(通常为导航控制器)；
//     消费方只连接信号，不持有、不删除源。源不持有任何消费方。
//   - 析构等价于先调用 stop()，但不再发出 statusChanged(Stopped)；析构后所有连接由 Qt 自动断开。
//   - 状态转换：Idle(构造后) → Starting → Active ⇄ NoFix；
//     任意运行态 → PermissionDenied / Unavailable(源自行停止产生位置，可再次 start() 重试)；
//     任意状态 → Stopped(stop() 或回放结束)；Stopped/PermissionDenied/Unavailable 可再次 start()。
//   - 每次 start() 生成新的 sessionId；PositionSample/LocationError 都携带 sessionId。
//
// ③ stop() 之后不得再产生回调
//   - stop() 同步执行：停止定时器/取消服务订阅，若当前不是 Stopped 则在返回前发出且仅发出一次
//     statusChanged(Stopped)；stop() 返回后，本对象不再发出 positionUpdated/statusChanged/errorOccurred，
//     直到下一次 start()。重复调用 stop() 为空操作。
//   - 跨线程排队连接的残留：stop() 前已排入消费方事件队列的信号仍可能在之后送达，
//     消费方必须比较 sessionId 与当前会话并丢弃旧样本(旧任务结果不得覆盖当前导航)。
#include "contracts/LocationTypes.h"
#include <QObject>

namespace p1::contracts {

// 位置源运行状态。
enum class LocationStatus : std::uint8_t {
    Idle,             // 已构造，从未启动
    Starting,         // 已请求启动，尚未得到首个定位(真实源等待授权/首个 fix)
    Active,           // 正常产生位置
    NoFix,            // 运行中但暂时无有效位置(隧道、信号丢失)；恢复后回到 Active
    PermissionDenied, // 用户/系统拒绝定位授权，源已停止产生位置
    Unavailable,      // 定位服务不可用(GeoClue2 未运行、Mock 未加载轨迹等)，源已停止产生位置
    Stopped           // 已调用 stop() 或回放结束
};

// 错误类别，区分四类需要不同处理的失败。
enum class LocationErrorCode : std::uint8_t {
    PermissionDenied,   // 权限被拒：引导用户授权，不自动重试
    ServiceUnavailable, // 服务不可用：提示并可稍后重试
    PositionLost,       // 位置丢失：状态转 NoFix，导航保持并等待恢复
    AccuracyTooLow      // 精度过低：样本仍会发出，由消费方决定是否用于偏航判断
};

// | 字段        | 类型              | 单位           | 可选 | 说明                                  |
// | code        | LocationErrorCode | —              | 否   | 错误类别                              |
// | message     | QString           | —              | 否   | 面向开发者的可读说明(含原因/路径等)   |
// | timestampMs | int64_t           | Unix 毫秒(UTC) | 否   | 错误发生时刻(墙钟)                    |
// | sessionId   | uint64_t          | —              | 否   | 发生错误的会话；0 表示未启动的会话    |
// | source      | SourceKind        | —              | 否   | mock/real 来源标记                    |
struct LocationError {
    LocationErrorCode code = LocationErrorCode::ServiceUnavailable;
    QString message;
    std::int64_t timestampMs = 0;
    std::uint64_t sessionId = 0;
    SourceKind source = SourceKind::Mock;
};

inline const char* toString(LocationStatus status) {
    switch (status) {
    case LocationStatus::Idle: return "Idle";
    case LocationStatus::Starting: return "Starting";
    case LocationStatus::Active: return "Active";
    case LocationStatus::NoFix: return "NoFix";
    case LocationStatus::PermissionDenied: return "PermissionDenied";
    case LocationStatus::Unavailable: return "Unavailable";
    case LocationStatus::Stopped: return "Stopped";
    }
    return "?";
}

inline const char* toString(LocationErrorCode code) {
    switch (code) {
    case LocationErrorCode::PermissionDenied: return "PermissionDenied";
    case LocationErrorCode::ServiceUnavailable: return "ServiceUnavailable";
    case LocationErrorCode::PositionLost: return "PositionLost";
    case LocationErrorCode::AccuracyTooLow: return "AccuracyTooLow";
    }
    return "?";
}

// 位置源抽象接口。实现类须遵守文件头的线程、所有权与停止语义。
// 注意：本头文件含 Q_OBJECT，其 moc 产物编译进 p1_navigation 库；使用方链接 p1_navigation。
class LocationSource : public QObject {
    Q_OBJECT
public:
    explicit LocationSource(QObject* parent = nullptr) : QObject(parent) {}
    ~LocationSource() override = default;

    // 开始产生位置。新会话 sessionId 递增；已在运行时为空操作。
    virtual void start() = 0;
    // 停止产生位置；返回后不再发出任何信号(见文件头 ③)。
    virtual void stop() = 0;
    virtual LocationStatus status() const = 0;
    virtual SourceKind kind() const = 0;
    // 当前会话 ID；从未启动时为 0。
    virtual std::uint64_t sessionId() const = 0;

signals:
    void positionUpdated(const p1::contracts::PositionSample& sample);
    void statusChanged(p1::contracts::LocationStatus status);
    void errorOccurred(const p1::contracts::LocationError& error);
};

} // namespace p1::contracts

Q_DECLARE_METATYPE(p1::contracts::LocationStatus)
Q_DECLARE_METATYPE(p1::contracts::LocationErrorCode)
Q_DECLARE_METATYPE(p1::contracts::LocationError)

namespace p1::contracts {
// 跨线程排队连接前调用一次(重复调用无害)。须位于 Q_DECLARE_METATYPE 之后。
inline void registerLocationMetaTypes() {
    qRegisterMetaType<p1::contracts::PositionSample>();
    qRegisterMetaType<p1::contracts::LocationStatus>();
    qRegisterMetaType<p1::contracts::LocationError>();
}
} // namespace p1::contracts
