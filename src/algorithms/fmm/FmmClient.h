#pragma once
#include "model/Types.h"
#include <QByteArray>
#include <QString>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace p1::algorithms {

// FMM uses signed external IDs (negative IDs can identify reverse roads),
// NOT P0 EdgeId or a dense array index.
struct FmmEdgeId { std::int64_t value{}; };
struct FmmPoint { double x{}, y{}; }; // dataset units, never implicitly metres
enum class FmmUnits { Metres, SyntheticSampleUnits };
enum class FmmStatus {
    Matched, WaitingForPoints, InvalidInput, InvalidConfiguration,
    NoMatch, ExecutionFailed, InvalidOutput, Cancelled, TimedOut
};

struct FmmFileSeal {
    QString path;
    qint64 size{}, mtime{};
    QByteArray sha256;
};
using FmmResourceSeal = std::array<FmmFileSeal, 4>;
using FmmResourceTimes = std::array<qint64, 4>;

struct FmmAuditRequest {
    std::optional<FmmResourceTimes> knownTimes;
    bool due{}, mayScan = true, blocked{};
};

struct FmmResourceReport {
    enum class Health { Unchecked, Verified, Corrupt, Deferred, ScanFailed };
    Health health = Health::Unchecked;
    QString reason;
    std::optional<FmmResourceTimes> times;
    bool scanned{}, backendTimeout{};
    qint64 scanMs{};
};

struct FmmConfig {
    QString executable, networkFile, ubodtFile;
    QString datasetId, networkVersion, ubodtNetworkVersion, coordinateDefinition;
    FmmUnits units = FmmUnits::Metres;
    int candidates = 8;
    double radius = 300, gpsError = 50; // same units as network, NOT degrees
    int timeoutMs = 30000;
    bool collectLoadTiming = false;
    std::shared_ptr<const FmmResourceSeal> resources;
    int auditIntervalMs = 30000;
    int auditBudgetMs = 30000;
    std::function<void()> auditStarted;
    // Optional explicit external-ID -> P0-ID table. Empty means separate ID
    // namespace; a supplied table must cover every returned road.
    std::map<std::int64_t, model::EdgeId> p0EdgeIds;
};

struct FmmResult {
    FmmStatus status = FmmStatus::InvalidInput;
    QString message, datasetId, dataVersion;
    QString sourceCommit = QStringLiteral("19ef34e1f57ff2f2484231aa0d01dfffea986ec1");
    bool synthetic = false;
    std::size_t inputPointCount{};
    double elapsedMs{}; // whole subprocess call incl. loading and parsing
    std::optional<double> ubodtLoadMs; // reported by the external backend, when available
    std::vector<FmmEdgeId> observationEdges, completePath;
    std::vector<model::EdgeId> p0ObservationEdges, p0CompletePath;
    std::vector<FmmPoint> projectedPoints, matchedGeometry;
    std::vector<double> errors; // distance to ROAD in dataset units, not route
    QString processLog; // diagnostics only; avoid persisting private GPS data
    QByteArray outputCsv; // fresh captured output; caller chooses whether to save it
    FmmResourceReport resource;
};

using FmmCancellation = std::shared_ptr<std::atomic_bool>;

// Blocking batch entry, call from a worker. Copies/config snapshots belong to
// the caller; no FMM library/Qt GUI object is shared with the child process.
FmmResult runFmm(const FmmConfig&, const std::vector<FmmPoint>&,
                 const FmmCancellation& cancel = {}, const FmmAuditRequest& audit = {});
// Strict parser for the selected pinned FMM CSV fields, also used in tests.
FmmResult parseFmmOutput(const QByteArray&, std::size_t expectedPoints);
bool parseFmmLineString(const QString&, std::vector<FmmPoint>&);

} // namespace p1::algorithms
