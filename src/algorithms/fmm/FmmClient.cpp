#include "fmm/FmmClient.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
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

static FmmResult runFmmProcess(const FmmConfig& config, const std::vector<FmmPoint>& points,
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
        QStringLiteral("--log_level"), config.collectLoadTiming ? QStringLiteral("2") : QStringLiteral("4")});
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
    if (config.collectLoadTiming) {
        static const QRegularExpression timing(
            QStringLiteral("Read UBODT file in ([0-9.eE+-]+) seconds"));
        const auto duration = timing.match(log);
        bool ok = false;
        const double seconds = duration.captured(1).toDouble(&ok);
        if (ok && std::isfinite(seconds) && seconds >= 0) result.ubodtLoadMs = seconds * 1000;
    }
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
namespace {
using Health = FmmResourceReport::Health;

std::optional<FmmResourceTimes> inspectFiles(const FmmResourceSeal& files, FmmResourceReport& report) {
    FmmResourceTimes times;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& file = files[i];
        const QFileInfo info(file.path);
        if (!info.isFile() || !info.isReadable() || info.size() != file.size) {
            report.health = Health::Corrupt;
            report.reason = QStringLiteral("会话资源缺失、不可读或长度不符：%1").arg(info.fileName());
            return {};
        }
        times[i] = info.lastModified().toMSecsSinceEpoch();
    }
    return times;
}

void verifyFiles(const FmmConfig& config, const FmmCancellation& cancel,
                 const FmmResourceTimes& before, FmmResourceReport& report) {
    report.scanned = true;
    QElapsedTimer timer;
    timer.start();
    if (config.auditStarted) config.auditStarted();
    auto check = [&] {
        report.scanMs = timer.elapsed();
        if (cancelled(cancel)) {
            report.health = Health::Deferred;
            report.reason = QStringLiteral("资源核验已取消");
            return false;
        }
        if (report.scanMs >= config.auditBudgetMs) {
            report.health = Health::ScanFailed;
            report.reason = QStringLiteral("资源完整性核验超过总时间预算");
            return false;
        }
        return true;
    };
    QByteArray buffer(256 * 1024, Qt::Uninitialized);
    bool changed = false;
    for (const auto& item : *config.resources) {
        if (!check()) return;
        QFile file(item.path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (inspectFiles(*config.resources, report)) {
                report.health = Health::ScanFailed;
                report.reason = QStringLiteral("暂时无法读取资源：%1").arg(file.errorString());
            }
            return;
        }
        QCryptographicHash sha(QCryptographicHash::Sha256);
        while (!file.atEnd()) {
            if (!check()) return;
            const auto count = file.read(buffer.data(), buffer.size());
            if (count <= 0) {
                report.health = Health::ScanFailed;
                report.reason = QStringLiteral("读取资源失败：%1").arg(file.errorString());
                return;
            }
            sha.addData(QByteArrayView(buffer.constData(), count));
        }
        if (sha.result() != item.sha256) changed = true;
    }
    if (!check()) return;
    const auto after = inspectFiles(*config.resources, report);
    if (!after) return;
    if (*after != before) {
        report.health = Health::ScanFailed;
        report.reason = QStringLiteral("核验期间资源元数据改变");
    } else if (changed) {
        report.health = Health::Corrupt;
        report.reason = QStringLiteral("会话资源 SHA-256 校验不符");
    } else {
        report.health = Health::Verified;
        report.times = after;
    }
}
}

FmmResult runFmm(const FmmConfig& config, const std::vector<FmmPoint>& points,
                 const FmmCancellation& cancel, const FmmAuditRequest& audit) {
    if (!config.resources) return runFmmProcess(config, points, cancel);
    FmmResourceReport report;
    auto failure = [&](FmmStatus status, const QString& reason) {
        FmmResult result;
        result.status = status;
        result.message = reason;
        result.datasetId = config.datasetId;
        result.dataVersion = config.networkVersion;
        result.inputPointCount = points.size();
        result.resource = report;
        return result;
    };
    const auto start = inspectFiles(*config.resources, report);
    if (!start) return failure(FmmStatus::InvalidConfiguration, report.reason);
    if (cancelled(cancel)) return failure(FmmStatus::Cancelled, QStringLiteral("Cancelled"));
    FmmResourceTimes known;
    for (std::size_t i = 0; i < known.size(); ++i) known[i] = (*config.resources)[i].mtime;
    if (audit.knownTimes) known = *audit.knownTimes;
    if (audit.due || *start != known || audit.blocked) {
        if (audit.blocked || !audit.mayScan) {
            report.health = Health::Deferred;
            report.reason = audit.blocked ? QStringLiteral("资源核验预算已用完，请停止并重新开始导航")
                                          : QStringLiteral("等待下一次资源完整性核验");
            return failure(FmmStatus::ExecutionFailed, report.reason);
        }
        verifyFiles(config, cancel, *start, report);
        if (report.health == Health::Corrupt) return failure(FmmStatus::InvalidConfiguration, report.reason);
        if (report.health != Health::Verified)
            return failure(cancelled(cancel) ? FmmStatus::Cancelled : FmmStatus::ExecutionFailed, report.reason);
    }
    auto result = runFmmProcess(config, points, cancel);
    report.backendTimeout = result.status == FmmStatus::TimedOut;
    const auto after = inspectFiles(*config.resources, report); // also after cancellation and child reaping
    if (!after) return failure(FmmStatus::InvalidConfiguration, report.reason);
    if (*after != *start) {
        report.health = Health::Deferred;
        report.times.reset();
        report.reason = QStringLiteral("匹配期间资源元数据改变，等待核验");
        return failure(FmmStatus::ExecutionFailed, report.reason);
    }
    result.resource = report;
    return result;
}
} // namespace p1::algorithms
