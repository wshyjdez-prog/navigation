#pragma once
// 4 号：WGS84(度) → 米制平面坐标 的确定性投影器。
// 对接 3 号 BoostRoutePlanner 的 CoordinateProjector（见 src/algorithms/qt/BoostRoutePlanner.h）。
//
// 采用等距圆柱投影（equirectangular）：以构造时固定的原点 (lat0, lon0) 为基准，
//   x_m = (lon - lon0) * metresPerDegLon
//   y_m = (lat - lat0) * metresPerDegLat
//
// 契约约束（文件头 ①：投影在 3 号 worker 线程执行）：
//   - 纯函数：无状态、无 GUI 依赖、线程安全（可被多 worker 并发调用）；
//   - 确定性：同一输入恒得同一输出，测试期望值可手算；
//   - 精度：3 号 locateNode 以 endpoint_tolerance_m(1e-6 m) 精确匹配图节点、
//     无最近邻吸附，故投影往返误差必须远小于 1e-6 m——双精度下本投影往返
//     误差量级 ~1e-13 m，安全裕度充足（tests/RealPlannerBridgeTests 覆盖）。
//
// ⚠️ 调用方须保证请求起终点的投影恰好落在路网节点上（真实 OSM 图由
// GraphData 构建器保证节点坐标与投影原点同源；样例图见 samples/FixedRoadNetwork.h）。
#include "contracts/NavigationTypes.h"
#include "model/Types.h"

#include <cmath>
#include <optional>

namespace p1::navigation {

class GeoProjector final {
public:
    // 原点与比例因子全部显式传入（米/度），不设隐含缺省——
    // 与 3 号契约注释一致："No implicit projection or speed. Provider must define both explicitly."
    GeoProjector(double originLatDeg, double originLonDeg,
                 double metresPerDegLat, double metresPerDegLon)
        : lat0_(originLatDeg), lon0_(originLonDeg),
          mLat_(metresPerDegLat), mLon_(metresPerDegLon) {}

    std::optional<p1::algorithms::model::MetricPoint> operator()(
        const p1::contracts::GeoCoordinate& coordinate) const {
        if (coordinate.crs != p1::contracts::CoordinateSystem::WGS84 ||
            !std::isfinite(coordinate.latitudeDeg) || !std::isfinite(coordinate.longitudeDeg)) {
            return std::nullopt; // 非法输入：交由 3 号按 InvalidNode 处理（契约错误路径）
        }
        p1::algorithms::model::MetricPoint point;
        point.x_m = (coordinate.longitudeDeg - lon0_) * mLon_;
        point.y_m = (coordinate.latitudeDeg - lat0_) * mLat_;
        return point;
    }

private:
    double lat0_;
    double lon0_;
    double mLat_;
    double mLon_;
};

} // namespace p1::navigation
