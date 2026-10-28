#pragma once
// 固定轨迹读取（4 号）。供 MockLocationSource 回放、3 号 HMM 样例和 5 号地图演示共用。
//
// CSV 格式（UTF-8，逗号分隔，不支持引号转义）：
//   # dataVersion=<必填，数据版本>
//   timestamp_ms,crs,lat_deg,lon_deg,alt_m,speed_mps,bearing_deg,h_accuracy_m,v_accuracy_m
//   1767225600000,WGS84,39.908700,116.397500,44.0,0.0,,8.0,12.0
//   - 以 # 开头的行是注释；“# 键=值”为元数据，目前识别 dataVersion。
//   - 必需列：timestamp_ms(Unix 毫秒)、crs(WGS84/GCJ02/BD09)、lat_deg、lon_deg(度)。
//   - 可选列：alt_m(米)、speed_mps(米/秒)、bearing_deg(度，正北 0 顺时针)、h_accuracy_m、v_accuracy_m(米)；
//     单元格留空表示该样本无此值。列名带单位，未知列名直接报错，避免单位写错被静默接受。
// JSON 格式：
//   {"format":"p1-fixed-track","formatVersion":1,"dataVersion":"...","crs":"WGS84",
//    "frames":[{"timestampMs":1767225600000,"latDeg":..,"lonDeg":..,"altM":..,"speedMps":..,
//               "bearingDeg":..,"hAccuracyM":..,"vAccuracyM":..}]}
//   可选字段可省略或为 null；未知字段报错。
// 统一校验：至少 1 帧；纬度[-90,90]、经度[-180,180]；时间戳 >0 且严格递增；全轨迹坐标系一致；
// 速度/精度 >=0；方向 [0,360)。
// 失败返回 TrackLoadResult::error（“来源:行号: 原因”），不抛异常、不静默跳过坏行。
#include "contracts/LocationTypes.h"
#include <QByteArray>
#include <QString>
#include <optional>
#include <vector>

namespace p1::navigation {

struct FixedTrack {
    QString sourceName;  // 文件路径或调用方给出的来源名，用于日志
    QString dataVersion; // 文件声明的数据版本，回放时写入 PositionSample::dataVersion
    contracts::CoordinateSystem crs = contracts::CoordinateSystem::WGS84;
    // 帧按时间升序；source=Mock，seq/sessionId/trackFrame 由回放时填写。
    std::vector<contracts::PositionSample> frames;
};

struct TrackLoadResult {
    std::optional<FixedTrack> track;
    QString error; // 成功时为空
    bool ok() const { return track.has_value(); }
};

class FixedTrackLoader {
public:
    // 按后缀 .csv/.json 选择格式；读取失败、格式不明均返回错误。
    static TrackLoadResult loadFile(const QString& path);
    static TrackLoadResult parseCsv(const QString& text, const QString& sourceName);
    static TrackLoadResult parseJson(const QByteArray& data, const QString& sourceName);
};

} // namespace p1::navigation
