#include "StubRoutePlanner.h"

#include <QString>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace p1::navigation {
namespace {
// stub 假设的巡航速度（米/秒）：仅用于从距离推算时长，确定性常量。
constexpr double kStubSpeedMps = 10.0;
constexpr double kEarthRadiusM = 6371008.8; // WGS84 平均半径
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
} // namespace

StubRoutePlanner::StubRoutePlanner(QObject* parent) : p1::contracts::RoutePlanner(parent) {}

std::uint64_t StubRoutePlanner::requestRoute(const p1::contracts::RoutePlanRequest& request) {
    p1::contracts::RoutePlanRequest effective = request;
    if (effective.requestId == 0) {
        effective.requestId = ++generatedRequestId_;
    }
    const std::uint64_t id = effective.requestId;
    if (honorCancel_ && cancelled_.count(id) > 0) {
        // 请求在发出前已被取消：不再受理，直接返回 0（契约：requestId<=0/非法时返回 0 的语义同源）。
        cancelled_.erase(id);
        return 0;
    }
    pending_[id] = effective;
    // 单发定时器运行于本对象所属（工作）线程的事件循环；delay=0 即下一个事件循环周期完成。
    QTimer::singleShot(computeDelayMs_, this, [this, id] { finishPlan(id); });
    return id;
}

void StubRoutePlanner::cancel(std::uint64_t requestId) {
    // honorCancel=true：移除待完成请求，finishPlan 无从回吐（契约语义）。
    // honorCancel=false（故障注入）：保留 pending_，迟到的结果照常回流，
    // 用于验证消费方（控制器）的陈旧结果拒绝链路；cancelled_ 仅作观测记录。
    if (honorCancel_) {
        pending_.erase(requestId);
    }
    cancelled_[requestId] = true;
}

void StubRoutePlanner::finishPlan(std::uint64_t requestId) {
    if (honorCancel_ && cancelled_.erase(requestId) > 0) {
        // 契约硬性规则：cancel 之后不得再回吐该 requestId 的结果。
        pending_.erase(requestId);
        return;
    }
    const auto it = pending_.find(requestId);
    if (it == pending_.end()) {
        return; // 未知或已完成的请求，忽略
    }
    const p1::contracts::RoutePlanRequest request = it->second;
    pending_.erase(it);

    if (failNextPlan_) {
        failNextPlan_ = false;
        emit routeFailed(requestId, failCode_, QStringLiteral("stub injected failure"));
        return;
    }
    emit routeReady(buildResult(request));
    emit planExecuted(requestId);
}

p1::contracts::RoutePlanResult StubRoutePlanner::buildResult(const p1::contracts::RoutePlanRequest& request) {
    p1::contracts::RoutePlanResult result;
    result.requestId = request.requestId;

    const double distanceM = haversineM(request.origin, request.destination);
    p1::contracts::RoutePlanRef route;
    route.routeId = QStringLiteral("stub-route-%1").arg(request.requestId);
    route.dataVersion = request.dataVersion;
    route.geometryRef = QStringLiteral("stub://%1,%2;%3,%4")
                            .arg(request.origin.latitudeDeg, 0, 'f', 6)
                            .arg(request.origin.longitudeDeg, 0, 'f', 6)
                            .arg(request.destination.latitudeDeg, 0, 'f', 6)
                            .arg(request.destination.longitudeDeg, 0, 'f', 6);
    route.totalDistanceM = distanceM;
    route.totalDurationSec = distanceM / kStubSpeedMps;
    route.source = p1::contracts::SourceKind::Mock;
    route.requestId = request.requestId;
    result.route = route;

    // 引导：出发 + 到达（真实引导生成属第 3 次作业，stub 只保证形状与确定性）。
    p1::contracts::GuidanceInstruction depart;
    depart.kind = p1::contracts::GuidanceKind::Depart;
    depart.text = QStringLiteral("Depart (stub)");
    depart.distanceToManeuverM = 0.0;
    depart.index = 0;
    p1::contracts::GuidanceInstruction arrive;
    arrive.kind = p1::contracts::GuidanceKind::ArriveDestination;
    arrive.text = QStringLiteral("Arrive at destination (stub)");
    arrive.distanceToManeuverM = distanceM;
    arrive.index = 1;
    result.instructions = {depart, arrive};
    return result;
}

double StubRoutePlanner::haversineM(const p1::contracts::GeoCoordinate& a, const p1::contracts::GeoCoordinate& b) {
    const double lat1 = a.latitudeDeg * kDegToRad;
    const double lat2 = b.latitudeDeg * kDegToRad;
    const double dLat = (b.latitudeDeg - a.latitudeDeg) * kDegToRad;
    const double dLon = (b.longitudeDeg - a.longitudeDeg) * kDegToRad;
    const double s = std::sin(dLat / 2) * std::sin(dLat / 2) +
                     std::cos(lat1) * std::cos(lat2) * std::sin(dLon / 2) * std::sin(dLon / 2);
    return 2 * kEarthRadiusM * std::asin(std::min(1.0, std::sqrt(s)));
}

} // namespace p1::navigation
