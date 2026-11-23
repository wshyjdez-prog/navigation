#include "NavigationWorkflow.h"
#include "NavigationRuntime.h"
#include "algorithms/qt/FmmMapMatcher.h"
#include "contracts/LocationSource.h"
#include "navigation/MockLocationSource.h"
#include <QElapsedTimer>
#include <QDateTime>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QLineF>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <utility>

namespace p1::app {
namespace {
using algorithms::FmmStatus;
using algorithms::FmmResourceReport;
using algorithms::model::GraphData;
using algorithms::model::EdgeId;
using algorithms::model::MetricPoint;
using algorithms::model::NodeId;

constexpr double maxEntryDistanceM = 60.0;
constexpr double maxFixDisplacementM = 40.0;

struct PreparedNavigation {
    MatchingNetwork matching;
    std::shared_ptr<const GraphData> graph;
    navigation::TrackLoadResult replay;
    FmmStatus failure = FmmStatus::ExecutionFailed;
    QString error;
};

struct EntryEdge {
    EdgeId id;
    NodeId from, to;
    MetricPoint fromPoint, toPoint;
    double roadLength{}, roadCost{}, length{}, prefixDistance{}, prefixRoadDistance{};
    std::size_t geometryOffset{};
    qsizetype geographicOffset{};
};

struct ReplannedRoute {
    algorithms::RoutePath path;
    QVector<QPointF> geographic;
    std::vector<EntryEdge> prefix;
    std::optional<EntryEdge> incoming;
    double anchor{};
    QString version, error;
};

QPointF geographic(const contracts::GeoCoordinate& point) {
    return {point.longitudeDeg, point.latitudeDeg};
}

std::optional<double> projectOnEdge(const EntryEdge& edge, QPointF point) {
    if (!(edge.length > 0) || !std::isfinite(edge.length) ||
        !std::isfinite(point.x()) || !std::isfinite(point.y())) return {};
    const QPointF from(edge.fromPoint.x_m, edge.fromPoint.y_m);
    const QPointF direction = (QPointF(edge.toPoint.x_m, edge.toPoint.y_m) - from) / edge.length;
    const double along = std::clamp(QPointF::dotProduct(point - from, direction), 0.0, edge.length);
    if (QLineF(point, from + direction * along).length() > 5.0) return {};
    return along;
}

bool addIncomingTail(ReplannedRoute& route, const EntryEdge& edge, QPointF point, QPointF matched) {
    auto& path = route.path;
    const auto along = projectOnEdge(edge, point);
    if (!along || path.nodes.empty() || path.edges.empty() || path.nodes.front() != edge.to ||
        path.geometry.size() < 2 || route.geographic.size() < 2 ||
        path.geometry.front().x_m != edge.toPoint.x_m || path.geometry.front().y_m != edge.toPoint.y_m)
        return false;

    const double remaining = edge.length - *along;
    if (remaining > maxEntryDistanceM || -remaining < route.anchor - 1.0) return false;
    const QPointF from(edge.fromPoint.x_m, edge.fromPoint.y_m);
    const QPointF to(edge.toPoint.x_m, edge.toPoint.y_m);
    // The FMM point is snapped to this edge. Allow only numeric rounding
    // when using the exact matched coordinate as the published first point.
    const QPointF projected = from + (to - from) * (*along / edge.length);
    if (QLineF(point, projected).length() > 0.001) return false;

    const MetricPoint start{point.x(), point.y()};
    if (remaining == 0.0) {
        path.geometry.front() = start;
        route.geographic.front() = matched;
        return true;
    }
    const double fraction = remaining / edge.length;
    const double distance = path.total_distance_m + fraction * edge.roadLength;
    const double cost = path.total_cost_m + fraction * edge.roadCost;
    if (!std::isfinite(distance) || !std::isfinite(cost)) return false;
    // Keep U -> V as the first directed leg; its published geometry starts
    // inside the edge and meets the already present junction V.
    path.nodes.insert(path.nodes.begin(), edge.from);
    path.edges.insert(path.edges.begin(), edge.id);
    path.geometry.insert(path.geometry.begin(), start);
    route.geographic.prepend(matched);
    path.total_distance_m = distance;
    path.total_cost_m = cost;
    return true;
}

// OSMX and the FMM export use the same two-point directed P0 edges.
void describeEntry(ReplannedRoute& result, const GraphData& graph, const RoadNetwork& network,
                   EdgeId trigger, QPointF point,
                   const algorithms::Cancellation& token) {
    constexpr std::size_t maxEdges = 16;
    const auto& path = result.path;
    const std::size_t count = std::min(path.edges.size(), maxEdges);
    if (!count) return;
    std::vector<EntryEdge> edges(count);
    std::optional<EntryEdge> triggerEdge;
    for (std::size_t i = 0; i < graph.edges.size(); ++i) {
        if (i % 1024 == 0 && token->load()) return;
        const auto& edge = graph.edges[i];
        const auto end = path.edges.begin() + count;
        const auto found = std::find(path.edges.begin(), end, edge.id);
        if (found == end && edge.id != trigger) continue;
        if (edge.geometry.size() != 2) return;
        const auto a = edge.geometry.front(), b = edge.geometry.back();
        const EntryEdge entry{edge.id, edge.from, edge.to, a, b, edge.length_m, edge.cost_m,
                              std::hypot(b.x_m - a.x_m, b.y_m - a.y_m)};
        if (found != end) edges[std::size_t(found - path.edges.begin())] = entry;
        if (edge.id == trigger) triggerEdge = entry;
    }
    if (!triggerEdge) return;
    const auto along = projectOnEdge(*triggerEdge, point);
    if (!along) return;
    // The trigger must anchor the first edge, or be able to reach its start
    // over the remainder of the same directed incoming edge.
    if (edges.front().id == trigger) result.anchor = *along;
    else if (triggerEdge->to == path.nodes.front() && triggerEdge->length - *along <= maxEntryDistanceM) {
        result.incoming = triggerEdge;
        result.anchor = *along - triggerEdge->length;
    } else return;
    double distance = 0, roadDistance = 0;
    std::size_t metricIndex = 0;
    qsizetype geoIndex = 0;
    const auto& start = path.geometry.front();
    auto lastGeo = network.unproject({start.x_m, start.y_m});
    for (std::size_t i = 0; i < edges.size() && distance - result.anchor <= maxEntryDistanceM; ++i) {
        auto& edge = edges[i];
        if (edge.id != path.edges[i] || !(edge.length > 0) || !std::isfinite(edge.length)) break;
        edge.prefixDistance = distance;
        edge.prefixRoadDistance = roadDistance;
        edge.geometryOffset = 2 * i;
        // geographicRoute coalesces consecutive points at edge junctions.
        while (metricIndex < edge.geometryOffset) {
            const auto& point = path.geometry[++metricIndex];
            const auto geo = network.unproject({point.x_m, point.y_m});
            if (geo != lastGeo) {
                ++geoIndex;
                lastGeo = geo;
            }
        }
        edge.geographicOffset = geoIndex;
        result.prefix.push_back(edge);
        distance += edge.length;
        roadDistance += edge.roadLength;
    }
}

bool validFix(const contracts::PositionSample& fix) {
    const auto& p = fix.coordinate;
    auto nonnegative = [](const std::optional<double>& value) {
        return !value || (std::isfinite(*value) && *value >= 0);
    };
    return fix.sessionId > 0 && fix.timestampMs > 0 && p.crs == contracts::CoordinateSystem::WGS84 &&
        std::isfinite(p.longitudeDeg) && std::abs(p.longitudeDeg) <= 180 &&
        std::isfinite(p.latitudeDeg) && std::abs(p.latitudeDeg) <= 90 &&
        (!p.altitudeM || std::isfinite(*p.altitudeM)) && nonnegative(fix.horizontalAccuracyM) &&
        nonnegative(fix.verticalAccuracyM) && nonnegative(fix.speedMps) &&
        nonnegative(fix.bearingDeg) && (!fix.bearingDeg || *fix.bearingDeg < 360);
}

QString matchMessage(FmmStatus status) {
    switch (status) {
    case FmmStatus::Matched: return QStringLiteral("FMM 匹配成功");
    case FmmStatus::WaitingForPoints: return QStringLiteral("等待更多定位点");
    case FmmStatus::NoMatch: return QStringLiteral("本窗口无匹配");
    case FmmStatus::InvalidInput: return QStringLiteral("无效定位或投影数据");
    case FmmStatus::InvalidConfiguration: return QStringLiteral("路网或 UBODT 配置无效");
    case FmmStatus::ExecutionFailed: return QStringLiteral("FMM 后端失败");
    case FmmStatus::InvalidOutput: return QStringLiteral("FMM 输出无效");
    case FmmStatus::Cancelled: return QStringLiteral("导航任务已取消");
    case FmmStatus::TimedOut: return QStringLiteral("FMM 后台任务超时");
    }
    return {};
}
}

struct NavigationWorkflow::Live {
    struct Context {
        quint64 session, epoch, revision;
        bool operator==(const Context& other) const {
            return session == other.session && epoch == other.epoch && revision == other.revision;
        }
    };
    struct ConfirmedFix {
        contracts::PositionSample fix;
        contracts::GeoCoordinate matched;
        EdgeId edge;
        double distance;
    };
    Context context() const { return {state.sessionId, fixEpoch, state.routeRevision}; }
    bool current(const Context& stamp) const { return state.active && !state.paused && context() == stamp; }
    bool latest(const contracts::PositionSample& fix) const {
        return lastFix && lastFix->sessionId == fix.sessionId && lastFix->seq == fix.seq &&
            lastFix->timestampMs == fix.timestampMs;
    }

    explicit Live(NavigationWorkflow& workflow) : owner(workflow) {
        contracts::registerNavigationMetaTypes();
        contracts::registerLocationMetaTypes();
        silence.setSingleShot(true);
        deadline.setSingleShot(true);
        connect(&silence, &QTimer::timeout, &owner, [this] { lost("定位中断，等待有效位置"); });
        connect(&deadline, &QTimer::timeout, &owner, [this] {
            if (state.rerouting) rejectReroute("重算或候选确认超时");
        });
        connect(&preparing, &QFutureWatcher<PreparedNavigation>::finished, &owner, [this] { prepared(); });
        connect(&routing, &QFutureWatcher<ReplannedRoute>::finished, &owner, [this] { replanned(); });
    }

    bool start(contracts::LocationSource* input, LiveNavigationOptions requested, QString playback = {}) {
        if (owner.busy_ || !owner.state_.network || !owner.state_.routeGeometry ||
            owner.state_.route.size() < 2 || !owner.state_.destination ||
            (input && input->thread() != owner.thread()) ||
            requested.fixGapMs <= 0 || requested.matchTimeoutMs <= 0 ||
            requested.resourceAuditIntervalMs <= 0 || requested.resourceAuditBudgetMs <= 0 ||
            requested.rerouteTimeoutMs <= 0 || requested.windowSize < 2 || requested.windowSize > 256)
            return false;
        stop(false);
        options = std::move(requested);
        state = {};
        state.active = true;
        state.sessionId = ++sessionCount;
        state.routeRevision = 1;
        network = owner.state_.network;
        destination = *owner.state_.destination;
        mode = owner.state_.routeMode;
        state.networkVersion = network->version + (mode == RouteMode::Driving ? "/car" : "/general");
        setRouteId();
        replayPath = std::move(playback);
        retryClock.invalidate();
        preparationAttempts = 0;
        owner.state_.observations.clear();
        owner.state_.matched.clear();
        if (input) bind(input);
        silence.start(options.fixGapMs);
        pump();
        return true;
    }

    void bind(contracts::LocationSource* input) {
        source = input;
        sourceConnections.push_back(connect(input, &contracts::LocationSource::positionUpdated,
            &owner, [this, input](const contracts::PositionSample& fix) {
                if (source == input && fix.sessionId == input->sessionId()) update(fix);
            }));
        sourceConnections.push_back(connect(input, &contracts::LocationSource::statusChanged,
            &owner, [this, input](contracts::LocationStatus status) {
                if (source != input) return;
                if (status == contracts::LocationStatus::NoFix ||
                    status == contracts::LocationStatus::Unavailable ||
                    status == contracts::LocationStatus::PermissionDenied)
                    lost("位置源暂不可用，保留当前路线");
            }));
    }

    void stop(bool publish = true) {
        if (!state.active) return;
        state.active = false;
        state.paused = state.locationLost = state.preparing = state.rerouting = false;
        silence.stop();
        resetFixes();
        retireMatcher();
        if (prepareToken) prepareToken->store(true);
        for (const auto& c : sourceConnections) disconnect(c);
        sourceConnections.clear();
        source.clear();
        if (replay) {
            replay->stop();
            replay.release()->deleteLater();
        }
        network.reset();
        lastFix.reset();
        lastReroute.reset();
        state.matchStatus = FmmStatus::Cancelled;
        if (publish) notify("连续导航已停止，保留当前路线");
    }

    void pause() {
        if (!state.active || state.paused) return;
        state.paused = true;
        state.locationLost = false;
        silence.stop();
        if (replay) replay->pause();
        resetFixes();
        state.matchStatus = FmmStatus::Cancelled;
        notify("连续导航已暂停");
    }

    void resume() {
        if (!state.active || !state.paused) return;
        state.paused = false;
        state.matchStatus = FmmStatus::WaitingForPoints;
        silence.start(options.fixGapMs);
        if (replay) replay->resume();
        notify("连续导航已恢复，等待定位");
        pump();
    }

    void update(const contracts::PositionSample& fix) {
        if (!state.active || state.paused) return;
        if (lastFix && (fix.sessionId < lastFix->sessionId ||
                        (fix.sessionId == lastFix->sessionId && fix.seq <= lastFix->seq)))
            return;
        if (!validFix(fix)) {
            resetFixes();
            state.matchStatus = FmmStatus::InvalidInput;
            notify(matchMessage(*state.matchStatus));
            return;
        }
        const bool newSourceSession = lastFix && fix.sessionId != lastFix->sessionId;
        if (lastFix && !newSourceSession && fix.timestampMs <= lastFix->timestampMs) {
            resetFixes();
            state.matchStatus = FmmStatus::InvalidInput;
            notify("定位时间戳未递增");
            return;
        }
        if (lastFix && (newSourceSession || fix.timestampMs - lastFix->timestampMs > options.fixGapMs))
            resetFixes();
        if (newSourceSession) lastReroute.reset();
        lastFix = fix;
        state.locationLost = false;
        state.match.reset();
        state.matchedEdge.reset();
        if (matcher && !retiring) state.matchStatus = FmmStatus::WaitingForPoints;
        silence.start(options.fixGapMs);
        pending = fix;
        pump();
        if (matcher && !retiring && latest(fix) && state.matchStatus == FmmStatus::WaitingForPoints)
            notify("当前定位待匹配");
    }

    void lost(const QString& message) {
        if (!state.active || state.paused || state.locationLost) return;
        silence.stop();
        state.locationLost = true;
        resetFixes();
        state.matchStatus = FmmStatus::WaitingForPoints;
        notify(message);
    }

    void resetFixes() {
        ++fixEpoch;
        pending.reset();
        state.match.reset();
        state.matchedEdge.reset();
        confirmed.reset();
        state.windowPoints = 0;
        clearDeviation();
        if (matcher) {
            matcher->resetWindow();
            if (matchId && !matchCancelClock.isValid()) matchCancelClock.start();
        }
        cancelReroute();
    }

    void cancelReroute() {
        if (routeToken && !routeToken->exchange(true)) routeCancelClock.start();
        deadline.stop();
        candidate.reset();
        state.rerouting = false;
    }

    void retireMatcher() {
        if (!matchId && matcher) matcher.release()->deleteLater();
        retiring = matchId != 0;
        roads.reset();
        state.resourceGeneration = 0;
    }

    void pump() {
        if (!state.active || state.paused || matchId || prepareToken || owner.state_.network != network) return;
        if (!matcher) {
            if (preparationAttempts >= 3) return;
            const int delay = preparationAttempts < 2 ? 1000 : 5000;
            if (!retryClock.isValid() || (pending && retryClock.elapsed() >= delay)) prepare();
            return;
        }
        if (!pending) return;
        matchFix = *std::exchange(pending, std::nullopt);
        matchContext = context();
        const contracts::MapMatchRequest request{++requestCount, matchFix.coordinate, matchFix.seq, state.routeId};
        matchId = matcher->requestMatch(request);
        if (!matchId) {
            clearDeviation();
            state.matchStatus = FmmStatus::InvalidInput;
            notify("匹配器拒绝定位样本");
        }
    }

    void prepare() {
        prepareToken = std::make_shared<std::atomic_bool>(false);
        const auto token = prepareToken;
        const auto snapshot = network;
        const auto config = options;
        const auto version = state.networkVersion;
        const auto profile = mode;
        const auto path = replayPath;
        prepSession = state.sessionId;
        ++preparationAttempts;
        ++state.preparationCount;
        state.preparing = true;
        preparing.setFuture(QtConcurrent::run(&owner.pool_, [token, snapshot, config, version, profile, path] {
            PreparedNavigation result;
            try {
                if (!path.isEmpty()) {
                    if (!QFileInfo(path).isFile() || QFileInfo(path).size() > 4 * 1024 * 1024) {
                        result.failure = FmmStatus::InvalidInput;
                        result.error = QStringLiteral("回放轨迹不可读或超过 4 MiB");
                        return result;
                    }
                    result.replay = navigation::FixedTrackLoader::loadFile(path);
                    if (!result.replay.ok() || result.replay.track->crs != contracts::CoordinateSystem::WGS84) {
                        result.failure = FmmStatus::InvalidInput;
                        result.error = result.replay.ok() ? QStringLiteral("导航回放需要 WGS84 轨迹") : result.replay.error;
                        return result;
                    }
                }
                if (profile == RouteMode::Driving)
                    result.graph = std::make_shared<const GraphData>(NavigationWorkflow::drivingGraph(*snapshot, token));
                else result.graph = std::shared_ptr<const GraphData>(snapshot, &snapshot->graph);
                result.matching = prepareMatchingNetwork(*snapshot, *result.graph, version, token,
                    config.fmmExecutable, config.ubodtExecutable, config.matchTimeoutMs);
            } catch (const MatchingNetworkError& e) {
                result.failure = e.status;
                result.error = QString::fromUtf8(e.what());
            } catch (const std::exception& e) {
                result.error = QString::fromUtf8(e.what());
            }
            return result;
        }));
        notify("正在准备导航路网和 UBODT");
    }

    void prepared() {
        auto result = preparing.future().takeResult();
        const bool accepted = state.active && prepSession == state.sessionId && !prepareToken->load();
        prepareToken.reset();
        if (!accepted) { pump(); return; }
        state.preparing = false;
        replayPath.clear();
        if (result.replay.ok() && result.replay.track->crs == contracts::CoordinateSystem::WGS84) {
            replay = std::make_unique<navigation::MockLocationSource>(&owner);
            replay->setTrack(std::move(*result.replay.track));
            bind(replay.get());
            replay->start();
            if (state.paused) replay->pause();
        }
        if (!result.error.isEmpty()) {
            preparationFailed(result.failure, result.error);
            return;
        }
        roads = std::move(result.graph);
        const auto resources = result.matching.files;
        const auto snapshot = network;
        auto& config = result.matching.config;
        if (owner.state_.network != snapshot || config.networkVersion != state.networkVersion ||
            config.networkVersion != config.ubodtNetworkVersion) {
            preparationFailed(FmmStatus::InvalidConfiguration, "路网快照或版本已变化");
            return;
        }
        algorithms::FmmCoordinateTransform transform;
        transform.toNetwork = [snapshot, resources](const contracts::GeoCoordinate& fix) {
            // The worker retains the exported files until the child has exited.
            Q_UNUSED(resources);
            const auto p = snapshot->project(geographic(fix));
            return std::optional<algorithms::FmmPoint>{{p.x(), p.y()}};
        };
        transform.toWgs84 = [snapshot](algorithms::FmmPoint point) {
            const auto p = snapshot->unproject({point.x, point.y});
            return std::optional<contracts::GeoCoordinate>{{contracts::CoordinateSystem::WGS84, p.y(), p.x(), {}}};
        };
        config.auditIntervalMs = options.resourceAuditIntervalMs;
        config.auditBudgetMs = options.resourceAuditBudgetMs;
        config.auditStarted = options.resourceAuditStarted;
        const auto generation = ++resourceCount;
        state.resourceGeneration = generation;
        postPublishVerified = false;
        successfulSpan = false;
        firstMatch.invalidate();
        matcher = std::make_unique<algorithms::FmmMapMatcher>(
            std::move(config), std::move(transform), options.windowSize, &owner);
        matcher->setReferenceRoute(state.routeId, owner.state_.routeGeometry);
        connect(matcher.get(), &algorithms::FmmMapMatcher::matchReady, &owner,
            [this](const contracts::MapMatchResult& match) { matched(match); });
        const auto session = state.sessionId;
        connect(matcher.get(), &algorithms::FmmMapMatcher::resourceChecked, &owner,
            [this, snapshot, session, generation](quint64 id, const FmmResourceReport& report) {
                if (id == matchId && state.active && session == state.sessionId && generation == state.resourceGeneration &&
                    network == snapshot && owner.state_.network == snapshot) checkedResource(report);
            });
        connect(matcher.get(), &algorithms::FmmMapMatcher::settled, &owner,
            [this](quint64 id) { settled(id); });
        state.exportMs = result.matching.exportMs;
        state.ubodtMs = result.matching.ubodtMs;
        state.matchStatus = FmmStatus::WaitingForPoints;
        const auto stamp = context();
        notify(state.paused ? QStringLiteral("连续导航已暂停") : matchMessage(*state.matchStatus));
        if (current(stamp)) pump();
    }

    void preparationFailed(FmmStatus failure, const QString& reason) {
        retryClock.start();
        state.matchStatus = failure;
        const auto exhausted = preparationAttempts >= 3 ?
            QStringLiteral(" · 自动恢复次数已用完，请停止并重新开始导航") : QString{};
        notify(matchMessage(failure) + " · " + reason + exhausted);
    }

    void closeRecoveryIfHealthy() {
        if (postPublishVerified && successfulSpan) {
            preparationAttempts = 0;
            retryClock.invalidate();
        }
    }

    void checkedResource(const FmmResourceReport& report) {
        using Health = FmmResourceReport::Health;
        if (report.scanned) state.resourceScanMs = report.scanMs;
        if (report.health == Health::Verified) {
            postPublishVerified = true;
            closeRecoveryIfHealthy();
        }
        if (report.health != Health::Corrupt) return;
        resetFixes();
        if (!state.paused && !state.locationLost) pending = lastFix;
        retireMatcher();
        if (!preparationAttempts) {
            retryClock.invalidate();
            state.matchStatus = FmmStatus::InvalidConfiguration;
            notify(matchMessage(*state.matchStatus) + " · " + report.reason);
        } else preparationFailed(FmmStatus::InvalidConfiguration, report.reason);
    }

    void settled(quint64 id) {
        if (id != matchId) return;
        matchId = 0;
        if (matchCancelClock.isValid()) {
            if (matchContext.session == state.sessionId) state.cancellationMs = matchCancelClock.nsecsElapsed()/1e6;
            matchCancelClock.invalidate();
        }
        if (retiring) {
            retiring = false;
            matcher.release()->deleteLater();
            if (state.active && matchContext.session == state.sessionId && preparationAttempts)
                retryClock.start();
        }
        pump();
    }


    void matched(const contracts::MapMatchResult& match) {
        if (!current(matchContext) || matchId != match.requestId || retiring) return;
        const auto stamp = matchContext;
        const auto fix = matchFix;
        const auto detail = matcher->lastDetails();
        if (!detail || detail->requestId != matchId || detail->locationSeq != fix.seq ||
            match.locationSeq != fix.seq) return;
        const auto& batch = detail->batch;
        auto status = batch.dataVersion == state.networkVersion ? batch.status : FmmStatus::InvalidConfiguration;
        if (status == FmmStatus::Matched && (!match.matched || !match.distanceToRouteM ||
            !std::isfinite(*match.distanceToRouteM) || batch.p0ObservationEdges.empty()))
            status = FmmStatus::InvalidOutput;
        const bool valid = status == FmmStatus::Matched;
        const bool off = valid && *match.distanceToRouteM > 15.0;
        const bool gap = lastOff && (lastOff->sessionId != fix.sessionId || fix.seq <= lastOff->seq ||
            fix.seq - lastOff->seq != 1 || fix.timestampMs <= lastOff->timestampMs ||
            fix.timestampMs - lastOff->timestampMs > options.fixGapMs);
        if (valid) {
            if (!firstMatch.isValid()) firstMatch.start();
            else if (firstMatch.elapsed() >= 5000) successfulSpan = true;
            closeRecoveryIfHealthy();
            confirmed = ConfirmedFix{fix, *match.matched, batch.p0ObservationEdges.back(), *match.distanceToRouteM};
            auto append = [this](QVector<QPointF>& trail, QPointF point) {
                trail.append(point);
                if (std::size_t(trail.size()) > options.windowSize) trail.removeFirst();
            };
            append(owner.state_.observations, geographic(fix.coordinate));
            append(owner.state_.matched, geographic(*match.matched));
        } else confirmed.reset();
        if (!off || gap) clearDeviation();
        if (off) {
            if (!offSince) offSince = fix.timestampMs;
            lastOff = fix;
            ++offFrames;
        }
        if (latest(fix)) {
            state.matchStatus = status;
            state.matchMs = batch.elapsedMs;
            state.ubodtLoadMs = batch.ubodtLoadMs;
            state.windowPoints = batch.inputPointCount;
            state.match = valid ? std::optional{match} : std::nullopt;
            state.matchedEdge = valid ? std::optional{confirmed->edge} : std::nullopt;
        }
        if (state.rerouting && (!off || gap)) {
            rejectReroute(gap ? QStringLiteral("偏航证据缺帧") :
                valid ? QStringLiteral("已回到原路线") : QStringLiteral("偏航匹配未能确认"));
            if (!current(stamp) || matchId != match.requestId) return;
        }
        if (latest(fix)) {
            auto message = matchMessage(status);
            if (status != FmmStatus::Matched && !batch.message.isEmpty()) message += " · " + batch.message;
            notify(message);
        }
        if (!current(stamp) || matchId != match.requestId || !latest(fix)) return;
        if (candidate) tryCandidate();
        else if (offSince && offFrames >= 3 && fix.timestampMs - *offSince >= 3000)
            maybeReroute();
    }

    void maybeReroute() {
        if (routeToken || state.rerouting || !confirmed || !latest(confirmed->fix) || !roads || retiring ||
            (lastReroute && confirmed->fix.timestampMs - *lastReroute < 10000)) return;
        trigger = *confirmed;
        lastReroute = trigger.fix.timestampMs;
        rerouteId = ++requestCount;
        routeContext = context();
        routeToken = std::make_shared<std::atomic_bool>(false);
        const auto token = routeToken;
        const auto target = destination;
        const auto snapshot = network;
        const auto graph = roads;
        const auto version = state.networkVersion;
        const auto profile = mode;
        const auto ready = options.rerouteResultReady;
        const auto anchor = trigger;
        state.rerouting = true;
        state.rerouteError.clear();
        routeClock.start();
        deadline.start(std::min(6000, options.rerouteTimeoutMs));
        routing.setFuture(QtConcurrent::run(&owner.pool_, [snapshot, graph, anchor, target, version, profile, token, ready] {
            ReplannedRoute result;
            result.version = version;
            try {
                const auto origin = geographic(anchor.matched);
                result.path = planOnSnapshot(*snapshot, *graph, profile, origin, target, token);
                if (result.path.status == algorithms::RouteStatus::Success) {
                    result.geographic = geographicRoute(*snapshot, result.path.geometry);
                    if (result.geographic.size() < 2) result.error = "重算起点与目的地吸附到同一节点";
                    else describeEntry(result, *graph, *snapshot, anchor.edge, snapshot->project(origin), token);
                } else result.error = QString::fromStdString(result.path.message);
                if (ready) ready();
            } catch (const std::exception& e) { result.error = QString::fromUtf8(e.what()); }
            return result;
        }));
        contracts::RerouteRequest request;
        request.requestId = rerouteId;
        request.issuedAtMs = QDateTime::currentMSecsSinceEpoch();
        request.currentPosition = trigger.matched;
        request.destination = {contracts::CoordinateSystem::WGS84, target.y(), target.x(), {}};
        request.previousRouteId = state.routeId;
        request.locationSeq = trigger.fix.seq;
        emit owner.navigationRerouteRequested(request);
    }

    void replanned() {
        auto result = routing.future().takeResult();
        const bool cancelled = routeToken->load();
        routeToken.reset(); // physical slot, even for an old session
        if (routeCancelClock.isValid()) {
            if (routeContext.session == state.sessionId) state.cancellationMs = routeCancelClock.nsecsElapsed()/1e6;
            routeCancelClock.invalidate();
        }
        if (cancelled || !state.rerouting || !current(routeContext)) return;
        state.planMs = result.path.elapsed_ms;
        const auto remaining = std::min(6000, options.rerouteTimeoutMs) - routeClock.elapsed();
        if (remaining <= 0) return rejectReroute("重算候选已过期");
        if (result.version != state.networkVersion || owner.state_.network != network ||
            result.path.status != algorithms::RouteStatus::Success || !result.error.isEmpty()) {
            return rejectReroute(result.error.isEmpty() ? QStringLiteral("重算不可用或路网已变化") : result.error);
        }
        candidate = std::move(result);
        candidateClock.start();
        deadline.start(int(std::min<qint64>(remaining, 2000)));
        tryCandidate();
    }

    void tryCandidate() {
        if (!candidate || !current(routeContext)) return;
        if (routeClock.elapsed() >= std::min(6000, options.rerouteTimeoutMs) || candidateClock.elapsed() >= 2000)
            return rejectReroute("重算候选已过期");
        if (!confirmed || !latest(confirmed->fix)) return;
        const auto& fix = *confirmed;
        const auto delta = fix.fix.timestampMs - trigger.fix.timestampMs;
        const QPointF matched = geographic(fix.matched);
        const QPointF point = network->project(matched);
        const QPointF origin = network->project(geographic(trigger.matched));
        if (fix.fix.sessionId != trigger.fix.sessionId || fix.fix.seq < trigger.fix.seq ||
            delta < 0 || delta > 5000 || fix.distance <= 15.0 || !offSince ||
            *offSince > trigger.fix.timestampMs || QLineF(origin, point).length() > maxFixDisplacementM) {
            return rejectReroute("当前位置无法正向接入重算路线");
        }
        if (candidate->incoming && fix.edge == candidate->incoming->id) {
            if (candidate->prefix.empty() || !addIncomingTail(*candidate, *candidate->incoming, point, matched))
                return rejectReroute("当前位置无法正向接入重算路线");
            return applyCandidate(0);
        }
        for (const auto& edge : candidate->prefix) {
            if (edge.id != fix.edge) continue;
            const auto along = projectOnEdge(edge, point);
            if (!along) break;
            const double advance = edge.prefixDistance + *along - candidate->anchor;
            if (advance < -1.0 || advance > maxEntryDistanceM) break;
            const double ratio = *along / edge.length;
            auto& path = candidate->path;
            if ((edge.geometryOffset + 2 == path.geometry.size() && ratio >= 1.0) ||
                edge.geographicOffset + 1 >= candidate->geographic.size()) break;
            path.geometry.erase(path.geometry.begin(), path.geometry.begin() + edge.geometryOffset);
            candidate->geographic.remove(0, edge.geographicOffset);
            const MetricPoint start{edge.fromPoint.x_m + ratio * (edge.toPoint.x_m - edge.fromPoint.x_m),
                                    edge.fromPoint.y_m + ratio * (edge.toPoint.y_m - edge.fromPoint.y_m)};
            path.geometry.front() = start;
            candidate->geographic.front() = network->unproject({start.x_m, start.y_m});
            if (candidate->geographic[0] == candidate->geographic[1]) candidate->geographic.removeFirst();
            path.total_distance_m = std::max(0.0,
                path.total_distance_m - edge.prefixRoadDistance - ratio * edge.roadLength);
            return applyCandidate(edge.geometryOffset / 2 + (ratio >= 1.0 ? 1 : 0));
        }
        rejectReroute("当前位置无法正向接入重算路线");
    }

    void applyCandidate(std::size_t skippedEdges) {
        auto result = std::move(*candidate);
        const auto id = rerouteId;
        const auto newest = lastFix;
        owner.state_.route = std::move(result.geographic);
        owner.state_.distanceM = result.path.total_distance_m;
        owner.state_.count = int(result.path.edges.size() - skippedEdges);
        owner.state_.routeGeometry = std::make_shared<const std::vector<MetricPoint>>(std::move(result.path.geometry));
        resetFixes();
        pending = newest;
        ++state.routeRevision;
        setRouteId();
        matcher->setReferenceRoute(state.routeId, owner.state_.routeGeometry);
        state.matchStatus = FmmStatus::WaitingForPoints;
        contracts::RoutePlanRef route;
        route.routeId = route.geometryRef = state.routeId;
        route.dataVersion = state.networkVersion;
        route.totalDistanceM = owner.state_.distanceM;
        route.source = contracts::SourceKind::Real;
        route.requestId = id;
        const auto stamp = context();
        emit owner.navigationRerouteFinished({id, route, {}}, {});
        if (!current(stamp)) return;
        if (newest && latest(*newest)) notify("偏航重算成功，已应用新路线");
        if (current(stamp)) pump();
    }

    void rejectReroute(const QString& message) {
        cancelReroute();
        state.rerouteError = message;
        const auto stamp = context();
        const auto newest = lastFix;
        emit owner.navigationRerouteFinished({rerouteId, {}, {}}, message);
        if (current(stamp) && (!newest || latest(*newest)))
            notify("偏航重算未应用，保留原路线 · " + message);
    }


    void clearDeviation() { offSince.reset(); lastOff.reset(); offFrames = 0; }

    void setRouteId() {
        state.routeId = QStringLiteral("%1/%2").arg(state.sessionId).arg(state.routeRevision);
    }

    void notify(const QString& message) {
        const auto stamp = context();
        const auto publication = ++notificationId;
        owner.status_ = message;
        owner.metrics_ = QString("路网导出 %1 ms · UBODT 生成 %2 ms · 加载 %3 ms · FMM %4 ms · A* %5 ms · 取消 %6 ms")
            .arg(state.exportMs,0,'f',1).arg(state.ubodtMs,0,'f',1)
            .arg(state.ubodtLoadMs ? QString::number(*state.ubodtLoadMs,'f',1) : QStringLiteral("—"))
            .arg(state.matchMs,0,'f',1).arg(state.planMs,0,'f',1).arg(state.cancellationMs,0,'f',1);
        emit owner.navigationMatchUpdated();
        if (publication == notificationId && context() == stamp) emit owner.changed();
    }

    NavigationWorkflow& owner;
    LiveNavigationState state;
    LiveNavigationOptions options;
    QPointer<contracts::LocationSource> source;
    std::vector<QMetaObject::Connection> sourceConnections;
    std::unique_ptr<navigation::MockLocationSource> replay;
    QString replayPath;
    QTimer silence, deadline;
    QElapsedTimer retryClock, matchCancelClock, routeCancelClock, routeClock, candidateClock, firstMatch;
    QFutureWatcher<PreparedNavigation> preparing;
    QFutureWatcher<ReplannedRoute> routing;
    algorithms::Cancellation prepareToken, routeToken;
    std::shared_ptr<const RoadNetwork> network;
    std::shared_ptr<const GraphData> roads;
    std::unique_ptr<algorithms::FmmMapMatcher> matcher;
    std::optional<contracts::PositionSample> lastFix, pending;
    contracts::PositionSample matchFix;
    std::optional<ConfirmedFix> confirmed;
    ConfirmedFix trigger;
    std::optional<ReplannedRoute> candidate;
    std::optional<contracts::PositionSample> lastOff;
    std::optional<std::int64_t> offSince, lastReroute;
    std::size_t offFrames{};
    QPointF destination;
    RouteMode mode{};
    bool retiring{};
    bool postPublishVerified{}, successfulSpan{};
    unsigned preparationAttempts{};
    quint64 sessionCount{}, requestCount{}, prepSession{}, fixEpoch{}, matchId{}, notificationId{}, rerouteId{}, resourceCount{};
    Context matchContext{}, routeContext{};
};

NavigationWorkflow::NavigationWorkflow(QObject* parent) : QObject(parent), live_(std::make_unique<Live>(*this)) {
    pool_.setMaxThreadCount(2);
}

NavigationWorkflow::~NavigationWorkflow() {
    live_->stop(false);
    for (const auto& token : cancellations_) token->store(true);
    pool_.waitForDone();
}

bool NavigationWorkflow::startNavigation(contracts::LocationSource* source, const LiveNavigationOptions& options) {
    return QThread::currentThread() == thread() && live_->start(source, options);
}

bool NavigationWorkflow::startNavigationReplay(const QString& trackFile) {
    return QThread::currentThread() == thread() && !trackFile.isEmpty() && live_->start(nullptr, {}, trackFile);
}

void NavigationWorkflow::locationUpdated(const contracts::PositionSample& fix) {
    if (QThread::currentThread() == thread()) live_->update(fix);
}

void NavigationWorkflow::pauseNavigation() { live_->pause(); }
void NavigationWorkflow::resumeNavigation() { live_->resume(); }
void NavigationWorkflow::stopNavigation() { live_->stop(); }
const LiveNavigationState& NavigationWorkflow::liveNavigation() const { return live_->state; }
int NavigationWorkflow::navigationTaskCount() const {
    return int(bool(live_->prepareToken)) + int(bool(live_->routeToken)) + int(live_->matchId != 0);
}

}
