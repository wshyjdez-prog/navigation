#include "fmm/FmmClient.h"
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <cmath>
#include <locale>
#include <sstream>

namespace p1::algorithms {
namespace {
bool finite(FmmPoint p) { return std::isfinite(p.x) && std::isfinite(p.y); }
bool cancelled(const FmmCancellation& c) { return c && c->load(); }
bool readIds(const QString& text, std::vector<FmmEdgeId>& out) {
    for (const auto& item : text.split(',')) {
        bool ok = false;
        const auto value = item.toLongLong(&ok);
        if (!ok) return false;
        out.push_back({value});
    }
    return true;
}
} // namespace

bool parseFmmLineString(const QString& text, std::vector<FmmPoint>& out) {
    out.clear();
    const auto wkt = text.trimmed();
    if (!wkt.startsWith(QStringLiteral("LINESTRING(")) || !wkt.endsWith(')')) return false;
    const auto body = wkt.mid(11, wkt.size() - 12);
    for (const auto& pair : body.split(',')) {
        std::istringstream in(pair.toStdString());
        in.imbue(std::locale::classic());
        FmmPoint p;
        if (!(in >> p.x >> p.y) || !finite(p)) return false;
        in >> std::ws;
        if (!in.eof()) return false;
        out.push_back(p);
    }
    return out.size() >= 2;
}

FmmResult parseFmmOutput(const QByteArray& bytes, std::size_t expectedPoints) {
    FmmResult result;
    result.outputCsv = bytes;
    result.status = FmmStatus::InvalidOutput;
    result.inputPointCount = expectedPoints;
    result.message = QStringLiteral("Malformed/incomplete FMM output");
    auto lines = QString::fromUtf8(bytes).split('\n');
    // Only trailing blank lines are permitted; this call writes exactly one
    // trajectory (id 1). Never read upstream's pre-existing mr.txt.
    while (!lines.empty() && lines.back().trimmed().isEmpty()) lines.removeLast();
    if (lines.size() != 2 || expectedPoints < 2) return result;
    const auto header = lines[0].trimmed().split(';');
    const auto fields = lines[1].trimmed().split(';');
    if (header.size() != 6 || header.size() != fields.size() ||
        header.count(QStringLiteral("id")) != 1) return result;
    for (const auto& key : {"opath", "cpath", "pgeom", "mgeom", "error"})
        if (header.count(QString::fromLatin1(key)) != 1) return result;
    auto field = [&](const char* key) { return fields[header.indexOf(QString::fromLatin1(key))]; };
    auto noMatch = [&] {
        result.status = FmmStatus::NoMatch;
        result.message = QStringLiteral("FMM found no complete HMM path");
        return result;
    };
    const bool emptyGeometry = field("mgeom") == QStringLiteral("LINESTRING()");
    const bool emptyObservations = field("opath").isEmpty() &&
        field("error").isEmpty() && field("pgeom").isEmpty();
    const auto id = field("id");
    // Pinned FMM returns MatchResult{} when there are no candidates. Its
    // writer emits result.id=0, empty observation/path fields and LINESTRING().
    // Accept ID 0 ONLY for this full structure, never for a partial/success row.
    if (id == QStringLiteral("0")) {
        if (emptyObservations && field("cpath").isEmpty() && emptyGeometry) return noMatch();
        return result;
    }
    if (id != QStringLiteral("1")) return result;
    if (emptyObservations && field("cpath").isEmpty())
        return emptyGeometry ? noMatch() : result;
    if (!readIds(field("opath"), result.observationEdges) ||
        !parseFmmLineString(field("pgeom"), result.projectedPoints)) return result;
    for (const auto& item : field("error").split(',')) {
        bool ok = false;
        const double value = item.toDouble(&ok);
        if (!ok || !std::isfinite(value) || value < 0) return result;
        result.errors.push_back(value);
    }
    if (result.observationEdges.size() != expectedPoints ||
        result.projectedPoints.size() != expectedPoints || result.errors.size() != expectedPoints)
        return result;
    // A disconnected complete path may still have valid per-observation
    // candidates. Validate them before reporting NoMatch, including ID 1.
    if (field("cpath").isEmpty()) return emptyGeometry ? noMatch() : result;
    if (!readIds(field("cpath"), result.completePath) ||
        !parseFmmLineString(field("mgeom"), result.matchedGeometry)) return result;
    result.status = FmmStatus::Matched;
    result.message = QStringLiteral("FMM HMM matched");
    return result;
}

FmmResult runFmm(const FmmConfig& config, const std::vector<FmmPoint>& points,
                 const FmmCancellation& cancel) {
    QElapsedTimer clock;
    clock.start();
    FmmResult result;
    auto finish = [&](FmmStatus status, const QString& message) {
        result.status = status;
        result.message = message;
        result.datasetId = config.datasetId;
        result.dataVersion = config.networkVersion;
        result.synthetic = config.units == FmmUnits::SyntheticSampleUnits;
        result.inputPointCount = points.size();
        result.elapsedMs = clock.nsecsElapsed() / 1e6;
        return result;
    };
    if (cancelled(cancel)) return finish(FmmStatus::Cancelled, QStringLiteral("Cancelled"));
    if (points.size() < 2 || points.size() > 256)
        return finish(FmmStatus::InvalidInput, QStringLiteral("Batch requires 2..256 ordered points"));
    for (const auto p : points)
        if (!finite(p)) return finish(FmmStatus::InvalidInput, QStringLiteral("Non-finite network coordinate"));
    if (config.datasetId.isEmpty() || config.networkVersion.isEmpty() ||
        config.networkVersion != config.ubodtNetworkVersion || config.coordinateDefinition.isEmpty() ||
        config.candidates <= 0 || !std::isfinite(config.radius) || config.radius <= 0 ||
        !std::isfinite(config.gpsError) || config.gpsError <= 0 || config.timeoutMs <= 0)
        return finish(FmmStatus::InvalidConfiguration, QStringLiteral("Check CRS, units, network/UBODT version and parameters"));
    for (const auto& path : {config.executable, config.networkFile, config.ubodtFile})
        if (!QFileInfo(path).isAbsolute() || !QFileInfo(path).isFile())
            return finish(FmmStatus::InvalidConfiguration, QStringLiteral("Missing absolute executable/network/UBODT path"));
    if (!QFileInfo(config.executable).isExecutable())
        return finish(FmmStatus::InvalidConfiguration, QStringLiteral("FMM program is not executable"));
    QTemporaryDir temp; // system local temp directory, removed after worker finishes
    if (!temp.isValid()) return finish(FmmStatus::ExecutionFailed, QStringLiteral("Cannot create local temporary directory"));
    const auto gpsPath = temp.filePath(QStringLiteral("trajectory.csv"));
    const auto outputPath = temp.filePath(QStringLiteral("matched.csv"));
    QByteArray input("id;geom\n1;LINESTRING(");
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (i) input += ',';
        input += QByteArray::number(points[i].x, 'g', 17) + ' ' + QByteArray::number(points[i].y, 'g', 17);
    }
    input += ")\n";
    QFile gps(gpsPath);
    if (!gps.open(QIODevice::WriteOnly) || gps.write(input) != input.size() || !gps.flush())
        return finish(FmmStatus::ExecutionFailed, QStringLiteral("Cannot write FMM input"));
    gps.close();
    QProcess process; // created and destroyed on THIS worker, never GUI thread
    process.setWorkingDirectory(temp.path());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(config.executable, {QStringLiteral("--network"), config.networkFile,
        QStringLiteral("--network_id"), QStringLiteral("id"),
        QStringLiteral("--source"), QStringLiteral("source"),
        QStringLiteral("--target"), QStringLiteral("target"),
        QStringLiteral("--ubodt"), config.ubodtFile,
        QStringLiteral("--gps"), gpsPath, QStringLiteral("--output"), outputPath,
        QStringLiteral("--output_fields"), QStringLiteral("opath,cpath,pgeom,mgeom,error"),
        QStringLiteral("--candidates"), QString::number(config.candidates),
        QStringLiteral("--radius"), QString::number(config.radius, 'g', 17),
        QStringLiteral("--error"), QString::number(config.gpsError, 'g', 17),
        QStringLiteral("--log_level"), QStringLiteral("4")}); // no shell, no ST-Matching
    if (!process.waitForStarted(qMin(config.timeoutMs, 3000)))
        return finish(FmmStatus::ExecutionFailed, process.errorString());
    auto collectLog = [&] {
        result.processLog += QString::fromUtf8(process.readAll());
        if (result.processLog.size() > 16384) result.processLog = result.processLog.right(16384);
    };
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(50);
        collectLog();
        if (cancelled(cancel) || clock.elapsed() >= config.timeoutMs) {
            process.kill();
            process.waitForFinished(); // reaped on worker, never GUI
            collectLog();
            return cancelled(cancel) ? finish(FmmStatus::Cancelled, QStringLiteral("Cancelled"))
                : finish(FmmStatus::TimedOut, QStringLiteral("FMM subprocess timeout"));
        }
    }
    collectLog();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return finish(FmmStatus::ExecutionFailed, QStringLiteral("FMM subprocess failed: %1").arg(process.exitCode()));
    QFile output(outputPath);
    // Upstream returns exit code 0 for some invalid configs: fresh output is mandatory.
    if (!output.open(QIODevice::ReadOnly) || output.size() > 4 * 1024 * 1024)
        return finish(FmmStatus::InvalidOutput, QStringLiteral("Missing or oversized fresh FMM output"));
    const auto log = result.processLog;
    result = parseFmmOutput(output.readAll(), points.size());
    result.processLog = log;
    if (result.status == FmmStatus::Matched && !config.p0EdgeIds.empty()) {
        auto mapIds = [&](const std::vector<FmmEdgeId>& ids, std::vector<model::EdgeId>& mapped) {
            for (const auto id : ids) {
                const auto found = config.p0EdgeIds.find(id.value);
                if (found == config.p0EdgeIds.end()) return false;
                mapped.push_back(found->second);
            }
            return true;
        };
        if (!mapIds(result.completePath, result.p0CompletePath) ||
            !mapIds(result.observationEdges, result.p0ObservationEdges))
            return finish(FmmStatus::InvalidOutput, QStringLiteral("FMM road ID missing from explicit P0 mapping"));
    }
    if (cancelled(cancel)) return finish(FmmStatus::Cancelled, QStringLiteral("Cancelled"));
    return finish(result.status, result.message);
}
} // namespace p1::algorithms
