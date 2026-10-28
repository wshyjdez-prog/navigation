#include "navigation/FixedTrackLoader.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStringList>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace p1::navigation {
namespace {
using contracts::CoordinateSystem;
using contracts::PositionSample;

TrackLoadResult fail(const QString& message) {
    TrackLoadResult result;
    result.error = message;
    return result;
}

bool parseFiniteDouble(const QString& text, double& out) {
    bool ok = false;
    out = text.toDouble(&ok);
    return ok && std::isfinite(out);
}

// 单帧字段范围校验；成功返回空串。
QString validateFrame(const PositionSample& s) {
    const auto& c = s.coordinate;
    if (c.latitudeDeg < -90.0 || c.latitudeDeg > 90.0)
        return QStringLiteral("纬度 %1 超出 [-90, 90] 度").arg(c.latitudeDeg, 0, 'f', 7);
    if (c.longitudeDeg < -180.0 || c.longitudeDeg > 180.0)
        return QStringLiteral("经度 %1 超出 [-180, 180] 度").arg(c.longitudeDeg, 0, 'f', 7);
    if (s.timestampMs <= 0)
        return QStringLiteral("时间戳 %1 必须是正的 Unix 毫秒").arg(s.timestampMs);
    if (s.speedMps && *s.speedMps < 0.0)
        return QStringLiteral("速度 %1 米/秒不能为负").arg(*s.speedMps);
    if (s.bearingDeg && (*s.bearingDeg < 0.0 || *s.bearingDeg >= 360.0))
        return QStringLiteral("方向 %1 度超出 [0, 360)").arg(*s.bearingDeg);
    if (s.horizontalAccuracyM && *s.horizontalAccuracyM < 0.0)
        return QStringLiteral("水平精度 %1 米不能为负").arg(*s.horizontalAccuracyM);
    if (s.verticalAccuracyM && *s.verticalAccuracyM < 0.0)
        return QStringLiteral("垂直精度 %1 米不能为负").arg(*s.verticalAccuracyM);
    return {};
}

// 校验并追加一帧，包括跨帧约束：时间严格递增、坐标系与首帧一致。
QString appendFrame(FixedTrack& track, const PositionSample& s) {
    const QString error = validateFrame(s);
    if (!error.isEmpty()) return error;
    if (track.frames.empty()) {
        track.crs = s.coordinate.crs;
    } else {
        if (s.coordinate.crs != track.crs)
            return QStringLiteral("坐标系 %1 与首帧 %2 不一致，同一轨迹不得混用坐标系")
                .arg(QLatin1String(contracts::toString(s.coordinate.crs)),
                     QLatin1String(contracts::toString(track.crs)));
        if (s.timestampMs <= track.frames.back().timestampMs)
            return QStringLiteral("时间戳 %1 未严格大于上一帧 %2")
                .arg(s.timestampMs).arg(track.frames.back().timestampMs);
    }
    track.frames.push_back(s);
    return {};
}

enum class Column { Timestamp, Crs, Lat, Lon, Alt, Speed, Bearing, HAccuracy, VAccuracy };
struct ColumnSpec { const char* name; Column column; bool required; };
constexpr ColumnSpec kCsvColumns[] = {
    {"timestamp_ms", Column::Timestamp, true},
    {"crs", Column::Crs, true},
    {"lat_deg", Column::Lat, true},
    {"lon_deg", Column::Lon, true},
    {"alt_m", Column::Alt, false},
    {"speed_mps", Column::Speed, false},
    {"bearing_deg", Column::Bearing, false},
    {"h_accuracy_m", Column::HAccuracy, false},
    {"v_accuracy_m", Column::VAccuracy, false},
};

// 可选数值：空 → 无值；非法 → 报错。
QString parseOptionalCell(const QString& cell, const char* column, std::optional<double>& out) {
    if (cell.isEmpty()) {
        out.reset();
        return {};
    }
    double value = 0.0;
    if (!parseFiniteDouble(cell, value))
        return QStringLiteral("列 %1 不是有效数值：“%2”").arg(QLatin1String(column), cell);
    out = value;
    return {};
}

QString parseRequiredCell(const QString& cell, const char* column, double& out) {
    if (cell.isEmpty()) return QStringLiteral("必需列 %1 为空").arg(QLatin1String(column));
    if (!parseFiniteDouble(cell, out))
        return QStringLiteral("列 %1 不是有效数值：“%2”").arg(QLatin1String(column), cell);
    return {};
}

QString parseCsvCell(const ColumnSpec& spec, const QString& cell, PositionSample& s) {
    switch (spec.column) {
    case Column::Timestamp: {
        bool ok = false;
        s.timestampMs = cell.toLongLong(&ok);
        if (!ok) return QStringLiteral("timestamp_ms 不是整数 Unix 毫秒：“%1”").arg(cell);
        return {};
    }
    case Column::Crs: {
        const auto crs = contracts::coordinateSystemFromString(cell);
        if (!crs) return QStringLiteral("无法识别的坐标系“%1”，应为 WGS84/GCJ02/BD09").arg(cell);
        s.coordinate.crs = *crs;
        return {};
    }
    case Column::Lat: return parseRequiredCell(cell, spec.name, s.coordinate.latitudeDeg);
    case Column::Lon: return parseRequiredCell(cell, spec.name, s.coordinate.longitudeDeg);
    case Column::Alt: return parseOptionalCell(cell, spec.name, s.coordinate.altitudeM);
    case Column::Speed: return parseOptionalCell(cell, spec.name, s.speedMps);
    case Column::Bearing: return parseOptionalCell(cell, spec.name, s.bearingDeg);
    case Column::HAccuracy: return parseOptionalCell(cell, spec.name, s.horizontalAccuracyM);
    case Column::VAccuracy: return parseOptionalCell(cell, spec.name, s.verticalAccuracyM);
    }
    return QStringLiteral("内部错误：未处理的列");
}

QString parseCsvHeader(const QStringList& cells, std::vector<const ColumnSpec*>& header) {
    for (const QString& raw : cells) {
        const QString name = raw.trimmed();
        const ColumnSpec* found = nullptr;
        for (const auto& spec : kCsvColumns)
            if (name == QLatin1String(spec.name)) found = &spec;
        if (!found)
            return QStringLiteral("未知列“%1”；列名须带单位，可用列见 FixedTrackLoader.h").arg(name);
        for (const auto* existing : header)
            if (existing == found) return QStringLiteral("重复列“%1”").arg(name);
        header.push_back(found);
    }
    for (const auto& spec : kCsvColumns) {
        if (!spec.required) continue;
        bool present = false;
        for (const auto* existing : header) present = present || existing == &spec;
        if (!present) return QStringLiteral("缺少必需列 %1").arg(QLatin1String(spec.name));
    }
    return {};
}

// JSON 可选数值：缺失或 null → 无值；非数值 → 报错。
QString readOptionalJson(const QJsonObject& o, const char* key, std::optional<double>& out) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isUndefined() || v.isNull()) {
        out.reset();
        return {};
    }
    if (!v.isDouble()) return QStringLiteral("%1 必须是数值或 null").arg(QLatin1String(key));
    out = v.toDouble();
    return {};
}

QString readRequiredJson(const QJsonObject& o, const char* key, double& out) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isDouble()) return QStringLiteral("缺少数值字段 %1").arg(QLatin1String(key));
    out = v.toDouble();
    return {};
}

QString unknownKey(const QJsonObject& o, std::initializer_list<const char*> known) {
    for (auto it = o.begin(); it != o.end(); ++it) {
        bool ok = false;
        for (const char* k : known) ok = ok || it.key() == QLatin1String(k);
        if (!ok) return it.key();
    }
    return {};
}
} // namespace

TrackLoadResult FixedTrackLoader::loadFile(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix != QLatin1String("csv") && suffix != QLatin1String("json"))
        return fail(QStringLiteral("%1: 不支持的轨迹格式“.%2”，仅支持 .csv / .json").arg(path, suffix));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("%1: 无法打开：%2").arg(path, file.errorString()));
    const QByteArray data = file.readAll();
    if (file.error() != QFileDevice::NoError)
        return fail(QStringLiteral("%1: 读取失败：%2").arg(path, file.errorString()));
    if (suffix == QLatin1String("csv")) return parseCsv(QString::fromUtf8(data), path);
    return parseJson(data, path);
}

TrackLoadResult FixedTrackLoader::parseCsv(const QString& text, const QString& sourceName) {
    FixedTrack track;
    track.sourceName = sourceName;
    std::vector<const ColumnSpec*> header;
    QString body = text;
    if (body.startsWith(QChar(0xFEFF))) body.remove(0, 1); // UTF-8 BOM
    const QStringList lines = body.split(QLatin1Char('\n'));
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const auto where = [&](const QString& message) {
            return fail(QStringLiteral("%1:%2: %3").arg(sourceName, QString::number(i + 1), message));
        };
        QString line = lines.at(i);
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        if (line.trimmed().isEmpty()) continue;
        if (line.startsWith(QLatin1Char('#'))) {
            const QString meta = line.mid(1).trimmed();
            const qsizetype eq = meta.indexOf(QLatin1Char('='));
            if (eq > 0 && meta.left(eq).trimmed() == QLatin1String("dataVersion"))
                track.dataVersion = meta.mid(eq + 1).trimmed();
            continue;
        }
        const QStringList cells = line.split(QLatin1Char(','));
        if (header.empty()) {
            const QString error = parseCsvHeader(cells, header);
            if (!error.isEmpty()) return where(error);
            continue;
        }
        if (static_cast<std::size_t>(cells.size()) != header.size())
            return where(QStringLiteral("列数 %1 与表头 %2 不一致").arg(cells.size()).arg(header.size()));
        PositionSample sample;
        sample.source = contracts::SourceKind::Mock;
        for (std::size_t c = 0; c < header.size(); ++c) {
            const QString error = parseCsvCell(*header[c], cells.at(static_cast<qsizetype>(c)).trimmed(), sample);
            if (!error.isEmpty()) return where(error);
        }
        const QString error = appendFrame(track, sample);
        if (!error.isEmpty()) return where(error);
    }
    if (track.dataVersion.isEmpty())
        return fail(QStringLiteral("%1: 缺少元数据行“# dataVersion=<版本>”").arg(sourceName));
    if (header.empty()) return fail(QStringLiteral("%1: 缺少表头").arg(sourceName));
    if (track.frames.empty()) return fail(QStringLiteral("%1: 没有任何轨迹帧").arg(sourceName));
    TrackLoadResult result;
    result.track = std::move(track);
    return result;
}

TrackLoadResult FixedTrackLoader::parseJson(const QByteArray& data, const QString& sourceName) {
    const auto error = [&](const QString& message) {
        return fail(QStringLiteral("%1: %2").arg(sourceName, message));
    };
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return error(QStringLiteral("JSON 解析失败（字节偏移 %1）：%2").arg(parseError.offset).arg(parseError.errorString()));
    if (!doc.isObject()) return error(QStringLiteral("顶层必须是 JSON 对象"));
    const QJsonObject root = doc.object();
    const QString extraRoot = unknownKey(root, {"format", "formatVersion", "dataVersion", "crs", "frames"});
    if (!extraRoot.isEmpty()) return error(QStringLiteral("未知字段“%1”").arg(extraRoot));
    if (root.value(QLatin1String("format")).toString() != QLatin1String("p1-fixed-track"))
        return error(QStringLiteral("format 必须为“p1-fixed-track”"));
    if (root.value(QLatin1String("formatVersion")).toInt(-1) != 1)
        return error(QStringLiteral("不支持的 formatVersion，当前仅支持 1"));

    FixedTrack track;
    track.sourceName = sourceName;
    track.dataVersion = root.value(QLatin1String("dataVersion")).toString();
    if (track.dataVersion.isEmpty()) return error(QStringLiteral("缺少 dataVersion"));
    const auto crs = contracts::coordinateSystemFromString(root.value(QLatin1String("crs")).toString());
    if (!crs) return error(QStringLiteral("缺少或无法识别 crs，应为 WGS84/GCJ02/BD09，不得省略"));
    const QJsonValue framesValue = root.value(QLatin1String("frames"));
    if (!framesValue.isArray()) return error(QStringLiteral("frames 必须是数组"));
    const QJsonArray frames = framesValue.toArray();

    for (qsizetype i = 0; i < frames.size(); ++i) {
        const auto where = [&](const QString& message) {
            return error(QStringLiteral("frames[%1]: %2").arg(QString::number(i), message));
        };
        if (!frames.at(i).isObject()) return where(QStringLiteral("必须是对象"));
        const QJsonObject f = frames.at(i).toObject();
        const QString extra = unknownKey(f, {"timestampMs", "latDeg", "lonDeg", "altM", "speedMps",
                                             "bearingDeg", "hAccuracyM", "vAccuracyM"});
        if (!extra.isEmpty()) return where(QStringLiteral("未知字段“%1”").arg(extra));

        PositionSample sample;
        sample.source = contracts::SourceKind::Mock;
        sample.coordinate.crs = *crs;
        const QJsonValue t = f.value(QLatin1String("timestampMs"));
        const double tMs = t.toDouble();
        // JSON 数字为 double，2^53 以内的整数毫秒可精确表示（覆盖到公元 28 万年）。
        if (!t.isDouble() || tMs != std::floor(tMs) || std::fabs(tMs) > 9007199254740992.0)
            return where(QStringLiteral("timestampMs 必须是整数 Unix 毫秒"));
        sample.timestampMs = static_cast<std::int64_t>(tMs);
        for (const QString& e : {readRequiredJson(f, "latDeg", sample.coordinate.latitudeDeg),
                                 readRequiredJson(f, "lonDeg", sample.coordinate.longitudeDeg),
                                 readOptionalJson(f, "altM", sample.coordinate.altitudeM),
                                 readOptionalJson(f, "speedMps", sample.speedMps),
                                 readOptionalJson(f, "bearingDeg", sample.bearingDeg),
                                 readOptionalJson(f, "hAccuracyM", sample.horizontalAccuracyM),
                                 readOptionalJson(f, "vAccuracyM", sample.verticalAccuracyM)})
            if (!e.isEmpty()) return where(e);
        const QString appendError = appendFrame(track, sample);
        if (!appendError.isEmpty()) return where(appendError);
    }
    if (track.frames.empty()) return error(QStringLiteral("没有任何轨迹帧"));
    TrackLoadResult result;
    result.track = std::move(track);
    return result;
}

} // namespace p1::navigation
