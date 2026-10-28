#pragma once
// 位置数据公共契约（4 号设计，1 号维护 include/contracts/）。
// 统一约定：距离米、速度米/秒、角度度、时间戳 Unix 毫秒(int64_t)；坐标必须带坐标系标记。
// 核心数据结构只用 C++17 标准库；Qt 仅用于字符串(QString)和信号槽元类型注册。
// 仅使用 Qt 6.4.2 已有 API。
#include <QMetaType>
#include <QString>
#include <cstdint>
#include <optional>

namespace p1::contracts {

// 坐标系。必须显式标记，不得省略；固定样例默认 WGS84。
// 本契约不做坐标系转换：消费方发现与自身坐标系不一致时应拒绝或显式转换，不得默认当作 WGS84。
enum class CoordinateSystem : std::uint8_t {
    WGS84, // GPS/GeoClue2 原生坐标系；固定样例默认值
    GCJ02, // 国测局加密坐标系
    BD09   // 百度坐标系
};

// 数据来源。Mock 数据不得作为真实定位或 PAL 适配的验收证据。
enum class SourceKind : std::uint8_t {
    Mock, // 固定轨迹回放/模拟
    Real  // 真实定位服务（GeoClue2，第 3 次作业接入）
};

// 地理坐标。
// | 字段         | 类型                  | 单位          | 可选 | 说明                         |
// | crs          | CoordinateSystem      | —             | 否   | 坐标系，必须显式填写         |
// | latitudeDeg  | double                | 度            | 否   | 纬度，[-90, 90]，北正南负    |
// | longitudeDeg | double                | 度            | 否   | 经度，[-180, 180]，东正西负  |
// | altitudeM    | std::optional<double> | 米            | 是   | 海拔；定位源未提供时为空     |
struct GeoCoordinate {
    CoordinateSystem crs = CoordinateSystem::WGS84;
    double latitudeDeg = 0.0;
    double longitudeDeg = 0.0;
    std::optional<double> altitudeM;
};

// 单个位置样本，由 LocationSource::positionUpdated 发出，按值复制传递，不引用源内部状态。
// | 字段               | 类型                          | 单位            | 可选 | 说明                                                      |
// | coordinate         | GeoCoordinate                 | 见上表          | 否   | 位置与坐标系                                              |
// | timestampMs        | int64_t                       | Unix 毫秒(UTC)  | 否   | 定位时刻；Mock 为轨迹文件中的时刻，不是回放时的墙钟时间   |
// | speedMps           | std::optional<double>         | 米/秒           | 是   | 对地速度，>= 0                                            |
// | bearingDeg         | std::optional<double>         | 度              | 是   | 行进方向，正北 0、顺时针，[0, 360)                        |
// | horizontalAccuracyM| std::optional<double>         | 米              | 是   | 水平精度半径，>= 0，越小越准                              |
// | verticalAccuracyM  | std::optional<double>         | 米              | 是   | 垂直精度，>= 0                                            |
// | source             | SourceKind                    | —               | 否   | mock/real 来源标记                                        |
// | seq                | uint64_t                      | 序号            | 否   | 同一会话内严格单调递增的发出序号，从 0 开始；seek 回退也继续递增 |
// | sessionId          | uint64_t                      | —               | 否   | 本次 start() 会话 ID（相当于统一约定中的 requestId）；每次 start() 递增，消费方据此丢弃旧会话样本 |
// | dataVersion        | QString                       | —               | 否   | 数据版本；Mock 为轨迹文件声明的版本，Real 为服务/适配器版本 |
// | trackFrame         | std::optional<uint64_t>       | 帧号            | 是   | 仅 Mock：该样本对应轨迹文件中的第几帧（0 起）；Real 为空  |
struct PositionSample {
    GeoCoordinate coordinate;
    std::int64_t timestampMs = 0;
    std::optional<double> speedMps;
    std::optional<double> bearingDeg;
    std::optional<double> horizontalAccuracyM;
    std::optional<double> verticalAccuracyM;
    SourceKind source = SourceKind::Mock;
    std::uint64_t seq = 0;
    std::uint64_t sessionId = 0;
    QString dataVersion;
    std::optional<std::uint64_t> trackFrame;
};

inline const char* toString(CoordinateSystem crs) {
    switch (crs) {
    case CoordinateSystem::WGS84: return "WGS84";
    case CoordinateSystem::GCJ02: return "GCJ02";
    case CoordinateSystem::BD09: return "BD09";
    }
    return "?";
}

inline const char* toString(SourceKind kind) {
    return kind == SourceKind::Mock ? "mock" : "real";
}

// 解析坐标系名称（大小写不敏感）；无法识别时返回空，调用方必须报错，不得回退为 WGS84。
inline std::optional<CoordinateSystem> coordinateSystemFromString(const QString& name) {
    const QString n = name.trimmed().toUpper();
    if (n == QLatin1String("WGS84")) return CoordinateSystem::WGS84;
    if (n == QLatin1String("GCJ02")) return CoordinateSystem::GCJ02;
    if (n == QLatin1String("BD09")) return CoordinateSystem::BD09;
    return std::nullopt;
}

} // namespace p1::contracts

Q_DECLARE_METATYPE(p1::contracts::CoordinateSystem)
Q_DECLARE_METATYPE(p1::contracts::SourceKind)
Q_DECLARE_METATYPE(p1::contracts::PositionSample)
