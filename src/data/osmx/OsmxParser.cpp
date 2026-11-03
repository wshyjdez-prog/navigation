#include "OsmxParser.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QUuid>
#include <QVarLengthArray>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace p1::data {
namespace {

constexpr char kMagic[4] = {'O', 'S', 'M', 'X'};
constexpr quint8 kFlagHasBounds = 0x01u;

struct ParseFailure {
    OsmxErrorCode code = OsmxErrorCode::CorruptRecord;
    QString message;
    qint64 offset = -1;
};

struct Cancelled {};

struct FileHeader {
    quint32 version = 0;
    QString sourcePath;
    quint8 flags = 0;
    OsmxBounds bounds;
    quint64 nodeCount = 0;
    qint64 headerEnd = 0;
};

class Cursor final {
public:
    Cursor(const uchar* base, qint64 size, qint64 start = 0)
        : base_(base), p_(base + start), end_(base + size) {}

    qint64 offset() const { return static_cast<qint64>(p_ - base_); }
    qint64 remaining() const { return static_cast<qint64>(end_ - p_); }
    const uchar* data() const { return p_; }

    void require(qint64 bytes) const {
        if (bytes < 0 || remaining() < bytes) {
            throw ParseFailure{OsmxErrorCode::Truncated,
                               QStringLiteral("文件在偏移 %1 处提前结束，仍缺少 %2 字节")
                                   .arg(offset())
                                   .arg(bytes),
                               offset()};
        }
    }
    void skip(qint64 bytes) {
        require(bytes);
        p_ += bytes;
    }
    quint8 u8() {
        require(1);
        return *p_++;
    }
    quint32 u32() {
        require(4);
        const quint32 value = qFromLittleEndian<quint32>(p_);
        p_ += 4;
        return value;
    }
    quint64 u64() {
        require(8);
        const quint64 value = qFromLittleEndian<quint64>(p_);
        p_ += 8;
        return value;
    }
    double f64() {
        const quint64 bits = u64();
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

private:
    const uchar* base_;
    const uchar* p_;
    const uchar* end_;
};

QString readString(Cursor& cursor) {
    const qint64 start = cursor.offset();
    const quint32 bytes = cursor.u32();
    if (bytes == 0) return QString();
    if ((bytes & 1u) != 0u) {
        throw ParseFailure{OsmxErrorCode::BadString,
                           QStringLiteral("字符串字节长度 %1 不是偶数（UTF-16LE 要求）").arg(bytes), start};
    }
    if (bytes > kOsmxMaxStringBytes) {
        throw ParseFailure{OsmxErrorCode::BadString,
                           QStringLiteral("字符串长度 %1 字节超过上限 %2").arg(bytes).arg(kOsmxMaxStringBytes),
                           start};
    }
    cursor.require(bytes);
    const uchar* raw = cursor.data();
    const qsizetype units = static_cast<qsizetype>(bytes / 2u);
    // OSMX packs strings directly after fixed-width fields, so the payload may
    // start at an odd address. Decode explicitly instead of casting to char16_t*.
    QVarLengthArray<char16_t, 256> text(units);
    for (qsizetype i = 0; i < units; ++i) {
        const quint16 unit = qFromLittleEndian<quint16>(raw + i * 2);
        if (unit == 0) {
            throw ParseFailure{OsmxErrorCode::BadString, QStringLiteral("字符串包含 NUL 字符"),
                               start + 4 + i * 2};
        }
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            const quint16 next = i + 1 < units ? qFromLittleEndian<quint16>(raw + (i + 1) * 2) : 0;
            if (next < 0xDC00 || next > 0xDFFF) {
                throw ParseFailure{OsmxErrorCode::BadString, QStringLiteral("UTF-16 高位代理未配对"),
                                   start + 4 + i * 2};
            }
            text[i] = static_cast<char16_t>(unit);
            text[i + 1] = static_cast<char16_t>(next);
            ++i;
            continue;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            throw ParseFailure{OsmxErrorCode::BadString, QStringLiteral("UTF-16 出现孤立低位代理"),
                               start + 4 + i * 2};
        }
        text[i] = static_cast<char16_t>(unit);
    }
    QString result = QString::fromUtf16(text.constData(), units);
    cursor.skip(bytes);
    return result;
}

void readHeader(Cursor& cursor, FileHeader& header) {
    cursor.require(12);
    if (std::memcmp(cursor.data(), kMagic, sizeof(kMagic)) != 0) {
        throw ParseFailure{OsmxErrorCode::BadMagic, QStringLiteral("文件头不是 OSMX 魔数"), 0};
    }
    cursor.skip(4);
    header.version = cursor.u32();
    if (header.version != kOsmxVersion) {
        throw ParseFailure{OsmxErrorCode::UnsupportedVersion,
                           QStringLiteral("不支持的 OSMX 版本 %1（当前仅支持 %2）")
                               .arg(header.version)
                               .arg(kOsmxVersion),
                           4};
    }
    header.sourcePath = readString(cursor);
    header.flags = cursor.u8();
    if ((header.flags & static_cast<quint8>(~kFlagHasBounds)) != 0u) {
        throw ParseFailure{OsmxErrorCode::UnsupportedFlags,
                           QStringLiteral("未知的头部标志位 0x%1").arg(header.flags, 2, 16, QLatin1Char('0')),
                           cursor.offset() - 1};
    }
    if ((header.flags & kFlagHasBounds) != 0u) {
        const double minLat = cursor.f64();
        const double minLon = cursor.f64();
        const double maxLat = cursor.f64();
        const double maxLon = cursor.f64();
        const bool finite = std::isfinite(minLat) && std::isfinite(minLon) && std::isfinite(maxLat) &&
                            std::isfinite(maxLon);
        if (!finite || minLat < -90.0 || maxLat > 90.0 || minLon < -180.0 || maxLon > 180.0 ||
            minLat > maxLat || minLon > maxLon) {
            throw ParseFailure{OsmxErrorCode::BadBounds, QStringLiteral("头部边界不是合法的 WGS84 矩形"),
                               cursor.offset() - 32};
        }
        header.bounds = OsmxBounds{minLat, minLon, maxLat, maxLon, true};
    }
    header.nodeCount = cursor.u64();
    if (header.nodeCount == 0) {
        throw ParseFailure{OsmxErrorCode::BadCount, QStringLiteral("OSMX 文件不包含节点"), cursor.offset() - 8};
    }
    if (header.nodeCount > std::numeric_limits<quint32>::max()) {
        throw ParseFailure{OsmxErrorCode::BadCount, QStringLiteral("节点数量超过 32 位索引上限"),
                           cursor.offset() - 8};
    }
    header.headerEnd = cursor.offset();
}

class StringPool final {
public:
    quint32 intern(const QString& value) {
        const auto found = ids_.constFind(value);
        if (found != ids_.constEnd()) return found.value();
        if (values_.size() >= static_cast<qsizetype>(std::numeric_limits<quint32>::max())) {
            throw ParseFailure{OsmxErrorCode::BadCount, QStringLiteral("字符串池超过 32 位索引上限"), -1};
        }
        const quint32 id = static_cast<quint32>(values_.size());
        values_.append(value);
        ids_.insert(value, id);
        return id;
    }
    const QStringList& values() const { return values_; }

private:
    QStringList values_;
    QHash<QString, quint32> ids_;
};

void readTags(Cursor& cursor, StringPool& pool, QVector<OsmxTag>& tags, quint32& first, quint32& count,
              const std::function<void()>& checkCancel) {
    first = static_cast<quint32>(tags.size());
    count = cursor.u32();
    if (count > static_cast<quint32>(cursor.remaining() / 8)) {
        throw ParseFailure{OsmxErrorCode::BadCount,
                           QStringLiteral("标签数量 %1 超出剩余字节允许范围").arg(count), cursor.offset() - 4};
    }
    if (static_cast<quint64>(tags.size()) + count > std::numeric_limits<quint32>::max()) {
        throw ParseFailure{OsmxErrorCode::BadCount, QStringLiteral("标签总数超过 32 位索引上限"),
                           cursor.offset() - 4};
    }
    for (quint32 i = 0; i < count; ++i) {
        if ((i & 0x3FFu) == 0u) checkCancel();
        const qint64 keyOffset = cursor.offset();
        const QString key = readString(cursor);
        if (key.isEmpty()) {
            throw ParseFailure{OsmxErrorCode::BadString, QStringLiteral("标签键为空"), keyOffset};
        }
        const QString value = readString(cursor);
        tags.append(OsmxTag{pool.intern(key), pool.intern(value)});
    }
}

bool tagIsNo(const QString& value) {
    const QString lowered = value.trimmed().toLower();
    return lowered == QLatin1String("no") || lowered == QLatin1String("false") ||
           lowered == QLatin1String("0");
}

std::optional<quint32> findTagValue(const QVector<OsmxTag>& tags, quint32 first, quint32 count,
                                    const QStringList& strings, QStringView key) {
    const quint32 end = first + count;
    for (quint32 i = first; i < end; ++i) {
        const OsmxTag& tag = tags.at(static_cast<int>(i));
        if (tag.key == kOsmxNoString) continue;
        if (strings.at(static_cast<int>(tag.key)) == key) return tag.value;
    }
    return std::nullopt;
}

quint16 parseSpeedKmh(const QString& raw) {
    QString value = raw.trimmed().toLower();
    if (value.isEmpty()) return 0;
    if (value == QLatin1String("none") || value == QLatin1String("signals") ||
        value == QLatin1String("variable") || value == QLatin1String("unknown")) {
        return 0;
    }
    if (value == QLatin1String("walk") || value == QLatin1String("walking")) return 5;
    double factor = 1.0;
    if (value.endsWith(QLatin1String("mph"))) {
        factor = 1.609344;
        value.chop(3);
    } else if (value.endsWith(QLatin1String("km/h"))) {
        value.chop(4);
    } else if (value.endsWith(QLatin1String("kmh"))) {
        value.chop(3);
    }
    value = value.trimmed();
    int digits = 0;
    while (digits < value.size()) {
        const QChar c = value.at(digits);
        if (!c.isDigit() && c != QLatin1Char('.') && c != QLatin1Char(',')) break;
        ++digits;
    }
    if (digits == 0) return 0;
    QString number = value.left(digits);
    number.replace(QLatin1Char(','), QLatin1Char('.'));
    bool ok = false;
    const double converted = number.toDouble(&ok);
    if (!ok || converted <= 0.0) return 0;
    const double kmh = converted * factor;
    if (kmh < 1.0) return 1;
    if (kmh > 300.0) return 300;
    return static_cast<quint16>(std::lround(kmh));
}

void fillAttributes(const QVector<OsmxTag>& tags, quint32 first, quint32 count, const QStringList& strings,
                    quint32 refCount, RoadAttributes& attributes) {
    auto valueOf = [&](QStringView key) -> std::optional<QString> {
        const auto index = findTagValue(tags, first, count, strings, key);
        if (!index.has_value() || *index == kOsmxNoString) return std::nullopt;
        return strings.at(static_cast<int>(*index));
    };
    auto stringIndex = [&](QStringView key) -> quint32 {
        const auto index = findTagValue(tags, first, count, strings, key);
        if (!index.has_value()) return kOsmxNoString;
        return *index;
    };
    auto positive = [&](QStringView key) {
        const auto value = valueOf(key);
        return value.has_value() && !tagIsNo(*value);
    };

    attributes = RoadAttributes{};
    const auto highway = valueOf(u"highway");
    if (highway.has_value()) attributes.roadClass = roadClassFromHighway(QStringView(*highway));

    const auto building = valueOf(u"building");
    const auto landuse = valueOf(u"landuse");
    const bool area = positive(u"area") || (building.has_value() && !tagIsNo(*building)) ||
                      (landuse.has_value() && !tagIsNo(*landuse));
    if (area) attributes.flags |= RoadArea;
    if (positive(u"bridge")) attributes.flags |= RoadBridge;
    if (positive(u"tunnel")) attributes.flags |= RoadTunnel;

    bool accessDenied = false;
    for (const QStringView key : {u"access", u"vehicle", u"motor_vehicle"}) {
        const auto value = valueOf(key);
        if (!value.has_value()) continue;
        const QString lowered = value->trimmed().toLower();
        if (lowered == QLatin1String("no") || lowered == QLatin1String("private")) accessDenied = true;
    }
    if (accessDenied) attributes.flags |= RoadAccessDenied;

    bool oneway = false;
    bool reversed = false;
    if (const auto value = valueOf(u"oneway"); value.has_value()) {
        const QString lowered = value->trimmed().toLower();
        if (lowered == QLatin1String("-1") || lowered == QLatin1String("reverse")) {
            oneway = true;
            reversed = true;
        } else if (lowered == QLatin1String("yes") || lowered == QLatin1String("true") ||
                   lowered == QLatin1String("1")) {
            oneway = true;
        }
    } else {
        const auto junction = valueOf(u"junction");
        const bool roundabout = junction.has_value() &&
                                junction->compare(QLatin1String("roundabout"), Qt::CaseInsensitive) == 0;
        const bool motorway = attributes.roadClass == RoadClass::Motorway ||
                              attributes.roadClass == RoadClass::MotorwayLink;
        oneway = roundabout || motorway;
    }
    if (oneway) {
        attributes.flags |= RoadOneway;
        if (reversed) attributes.flags |= RoadOnewayReversed;
    }

    if (const auto lanes = valueOf(u"lanes"); lanes.has_value()) {
        bool ok = false;
        const int parsed = lanes->trimmed().toInt(&ok);
        if (ok && parsed > 0) attributes.lanes = static_cast<quint8>(std::min(parsed, 255));
    }
    if (const auto speed = valueOf(u"maxspeed"); speed.has_value()) {
        attributes.maxSpeedKmh = parseSpeedKmh(*speed);
    }
    if (const auto layer = valueOf(u"layer"); layer.has_value()) {
        bool ok = false;
        const int parsed = layer->trimmed().toInt(&ok);
        if (ok) attributes.layer = static_cast<qint16>(std::clamp(parsed, -100, 100));
    }

    // Rendering label preference: Chinese name, then the common name, then the
    // English name. The raw tags remain available through wayTag().
    for (const QStringView key : {u"name:zh", u"name", u"name:en"}) {
        const quint32 index = stringIndex(key);
        if (index != kOsmxNoString && !strings.at(static_cast<int>(index)).isEmpty()) {
            attributes.name = index;
            break;
        }
    }

    if (roadClassIsRoutable(attributes.roadClass) && !area && !accessDenied && refCount >= 2) {
        attributes.flags |= RoadRoutable;
    }
}

void reportProgress(const OsmxParseCallbacks& callbacks, int& lastPercent, QString& lastStage, int percent,
                    const QString& stage) {
    percent = std::clamp(percent, 0, 100);
    if (percent < lastPercent) return;
    if (percent == lastPercent && stage == lastStage) return;
    lastPercent = percent;
    lastStage = stage;
    if (callbacks.progress) callbacks.progress(percent, stage);
}

} // namespace

QString OsmxParser::inputFormat(const QString& path) {
    const QString name = QFileInfo(path).fileName().toLower();
    if (name.endsWith(QLatin1String(".osmx"))) return QStringLiteral("osmx");
    return QString();
}

bool OsmxParser::supports(const QString& path) {
    return !inputFormat(path).isEmpty();
}

OsmxError OsmxParser::validateHeader(const QString& path) {
    OsmxError error;
    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable()) {
        error.code = OsmxErrorCode::OpenFailed;
        error.message = QStringLiteral("文件不存在或不可读");
        return error;
    }
    const qint64 size = info.size();
    if (size <= 0) {
        error.code = OsmxErrorCode::Truncated;
        error.message = QStringLiteral("文件为空");
        return error;
    }
    if (static_cast<quint64>(size) > kOsmxMaxFileBytes) {
        error.code = OsmxErrorCode::FileTooLarge;
        error.message = QStringLiteral("文件大小超过 %1 字节上限").arg(kOsmxMaxFileBytes);
        return error;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error.code = OsmxErrorCode::OpenFailed;
        error.message = file.errorString();
        return error;
    }
    // Read exactly the header instead of guessing a prefix size: the stored
    // source path is length-prefixed and may legitimately exceed any fixed
    // buffer, which a truncated prefix would misreport as a broken file.
    auto readAt = [&file](qint64 offset, qint64 count, QByteArray* out) {
        if (!file.seek(offset)) return false;
        *out = file.read(count);
        return out->size() == count;
    };
    QByteArray headerBytes;
    if (!readAt(0, 12, &headerBytes)) {
        error.code = OsmxErrorCode::Truncated;
        error.message = QStringLiteral("文件头不足 12 字节");
        return error;
    }
    const quint32 pathBytes = qFromLittleEndian<quint32>(headerBytes.constData() + 8);
    if ((pathBytes & 1u) != 0u) {
        error.code = OsmxErrorCode::BadString;
        error.message = QStringLiteral("头部来源路径字节长度不是偶数（UTF-16LE 要求）");
        error.offset = 8;
        return error;
    }
    if (pathBytes > kOsmxMaxStringBytes) {
        error.code = OsmxErrorCode::BadString;
        error.message = QStringLiteral("头部来源路径长度超过上限 %1 字节").arg(kOsmxMaxStringBytes);
        error.offset = 8;
        return error;
    }
    const qint64 flagsOffset = 12 + static_cast<qint64>(pathBytes);
    QByteArray pathPayload;
    if (pathBytes > 0 && !readAt(12, pathBytes, &pathPayload)) {
        error.code = OsmxErrorCode::Truncated;
        error.message = QStringLiteral("文件在头部来源路径内提前结束");
        error.offset = 12;
        return error;
    }
    QByteArray flagByte;
    if (!readAt(flagsOffset, 1, &flagByte)) {
        error.code = OsmxErrorCode::Truncated;
        error.message = QStringLiteral("文件在头部来源路径之后提前结束");
        error.offset = flagsOffset;
        return error;
    }
    const quint8 flags = static_cast<quint8>(flagByte.at(0));
    if ((flags & static_cast<quint8>(~kFlagHasBounds)) != 0u) {
        error.code = OsmxErrorCode::UnsupportedFlags;
        error.message = QStringLiteral("未知的头部标志位 0x%1").arg(flags, 2, 16, QLatin1Char('0'));
        error.offset = flagsOffset;
        return error;
    }
    const qint64 remainingHeader = ((flags & kFlagHasBounds) != 0u ? 32 : 0) + 8;
    QByteArray tail;
    if (!readAt(flagsOffset + 1, remainingHeader, &tail)) {
        error.code = OsmxErrorCode::Truncated;
        error.message = QStringLiteral("文件头不足：边界或节点计数缺失");
        error.offset = flagsOffset + 1;
        return error;
    }
    headerBytes += pathPayload;
    headerBytes += flagByte;
    headerBytes += tail;
    try {
        Cursor cursor(reinterpret_cast<const uchar*>(headerBytes.constData()), headerBytes.size());
        FileHeader header;
        readHeader(cursor, header);
        if (header.nodeCount > static_cast<quint64>((size - header.headerEnd) / 28)) {
            error.code = OsmxErrorCode::BadCount;
            error.message = QStringLiteral("节点数量与文件大小不符");
            error.offset = header.headerEnd;
            return error;
        }
    } catch (const ParseFailure& failure) {
        error.code = failure.code;
        error.message = failure.message;
        error.offset = failure.offset;
    }
    return error;
}

OsmxParseResult OsmxParser::parse(const QString& path, const OsmxParseOptions& options,
                                  const OsmxParseCallbacks& callbacks) const {
    OsmxParseResult result;
    QElapsedTimer timer;
    timer.start();
    int lastPercent = -1;
    QString lastStage;
    auto checkCancel = [&callbacks] {
        if (callbacks.cancelled && callbacks.cancelled()) throw Cancelled{};
    };

    const QFileInfo before(path);
    result.sourcePath = before.absoluteFilePath();
    result.fileBytes = before.size();
    if (!before.isFile() || !before.isReadable()) {
        result.error.code = OsmxErrorCode::OpenFailed;
        result.error.message = QStringLiteral("文件不存在或不可读：%1").arg(path);
        return result;
    }
    if (before.size() <= 0) {
        result.error.code = OsmxErrorCode::Truncated;
        result.error.message = QStringLiteral("文件为空");
        return result;
    }
    if (static_cast<quint64>(before.size()) > kOsmxMaxFileBytes) {
        result.error.code = OsmxErrorCode::FileTooLarge;
        result.error.message = QStringLiteral("文件大小超过 %1 字节上限").arg(kOsmxMaxFileBytes);
        return result;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error.code = OsmxErrorCode::OpenFailed;
        result.error.message = file.errorString();
        return result;
    }

    const qint64 size = file.size();
    uchar* mapped = file.map(0, size);
    QByteArray buffer;
    const uchar* base = mapped;
    if (base == nullptr) {
        buffer = file.readAll();
        if (buffer.size() != size) {
            result.error.code = OsmxErrorCode::IoError;
            result.error.message = QStringLiteral("读取文件失败：%1").arg(file.errorString());
            return result;
        }
        base = reinterpret_cast<const uchar*>(buffer.constData());
    }
    struct MappingGuard {
        QFile& file;
        uchar* mapped;
        ~MappingGuard() {
            if (mapped != nullptr) file.unmap(mapped);
        }
    } guard{file, mapped};

    try {
        reportProgress(callbacks, lastPercent, lastStage, 0, QStringLiteral("校验文件头"));
        Cursor cursor(base, size);
        FileHeader header;
        readHeader(cursor, header);
        if (header.nodeCount > static_cast<quint64>((size - header.headerEnd) / 28)) {
            throw ParseFailure{OsmxErrorCode::BadCount,
                               QStringLiteral("节点数量 %1 与文件大小不符").arg(header.nodeCount),
                               static_cast<qint64>(header.headerEnd)};
        }

        reportProgress(callbacks, lastPercent, lastStage, 1, QStringLiteral("校验源文件"));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (qint64 offset = 0; offset < size; offset += 4 * 1024 * 1024) {
            checkCancel();
            const qint64 chunk = std::min<qint64>(4 * 1024 * 1024, size - offset);
            hash.addData(QByteArrayView(reinterpret_cast<const char*>(base + offset), chunk));
            reportProgress(callbacks, lastPercent, lastStage, 1 + static_cast<int>(offset * 4 / size),
                           QStringLiteral("校验源文件"));
        }
        result.sha256 = QString::fromLatin1(hash.result().toHex());

        OsmxDataset::Data data;
        data.buildSpatialIndex = options.buildSpatialIndex;
        data.info.datasetId = options.datasetId.isEmpty()
                                  ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                                  : options.datasetId;
        data.info.sourceFileName = before.fileName();
        data.info.sourcePath = result.sourcePath;
        data.info.headerSourcePath = header.sourcePath;
        data.info.sourceSha256 = result.sha256;
        data.info.formatVersion = header.version;
        data.info.fileBytes = size;
        data.info.bounds = header.bounds;

        StringPool strings;
        strings.intern(QString()); // index 0 is always the empty string
        const quint64 nodeCount = header.nodeCount;
        data.nodes.reserve(static_cast<qsizetype>(nodeCount));
        QHash<qint64, quint32> nodeIds;
        nodeIds.reserve(static_cast<qsizetype>(nodeCount));
        OsmxBounds computedBounds;
        quint64 nodeTagCount = 0;
        for (quint64 i = 0; i < nodeCount; ++i) {
            if ((i & 0x3FFu) == 0u) checkCancel();
            OsmxNode node;
            node.id = static_cast<qint64>(cursor.u64());
            if (node.id == 0) {
                throw ParseFailure{OsmxErrorCode::CorruptRecord, QStringLiteral("节点 ID 不能为 0"),
                                   cursor.offset() - 8};
            }
            node.lat = cursor.f64();
            node.lon = cursor.f64();
            if (!std::isfinite(node.lat) || !std::isfinite(node.lon) || node.lat < -90.0 || node.lat > 90.0 ||
                node.lon < -180.0 || node.lon > 180.0) {
                throw ParseFailure{OsmxErrorCode::BadCoordinate,
                                   QStringLiteral("节点 %1 的经纬度超出 WGS84 范围").arg(node.id),
                                   cursor.offset() - 16};
            }
            if (nodeIds.contains(node.id)) {
                throw ParseFailure{OsmxErrorCode::DuplicateId, QStringLiteral("节点 ID 重复：%1").arg(node.id),
                                   cursor.offset() - 24};
            }
            quint32 first = 0;
            quint32 count = 0;
            readTags(cursor, strings, data.nodeTags, first, count, checkCancel);
            node.tagFirst = first;
            node.tagCount = count;
            nodeTagCount += count;
            nodeIds.insert(node.id, static_cast<quint32>(data.nodes.size()));
            data.nodes.append(node);
            if (!computedBounds.valid) {
                computedBounds = OsmxBounds{node.lat, node.lon, node.lat, node.lon, true};
            } else {
                computedBounds.minLat = std::min(computedBounds.minLat, node.lat);
                computedBounds.minLon = std::min(computedBounds.minLon, node.lon);
                computedBounds.maxLat = std::max(computedBounds.maxLat, node.lat);
                computedBounds.maxLon = std::max(computedBounds.maxLon, node.lon);
            }
            if ((i & 0xFFu) == 0u || i + 1 == nodeCount) {
                reportProgress(callbacks, lastPercent, lastStage,
                               5 + static_cast<int>(i * 55 / nodeCount), QStringLiteral("解析节点"));
            }
        }
        data.info.nodeTagCount = nodeTagCount;
        reportProgress(callbacks, lastPercent, lastStage, 60, QStringLiteral("解析道路"));

        const quint64 wayCount = cursor.u64();
        if (wayCount > static_cast<quint64>(cursor.remaining() / 12)) {
            throw ParseFailure{OsmxErrorCode::BadCount,
                               QStringLiteral("道路数量 %1 超出剩余字节允许范围").arg(wayCount),
                               cursor.offset() - 8};
        }
        if (wayCount > std::numeric_limits<quint32>::max()) {
            throw ParseFailure{OsmxErrorCode::BadCount, QStringLiteral("道路数量超过 32 位索引上限"),
                               cursor.offset() - 8};
        }
        data.ways.reserve(static_cast<qsizetype>(wayCount));
        QSet<qint64> wayIds;
        wayIds.reserve(static_cast<qsizetype>(wayCount));
        quint64 wayTagCount = 0;
        quint64 wayNodeReferences = 0;
        quint32 shortWays = 0;
        quint32 routableWayCount = 0;
        for (quint64 i = 0; i < wayCount; ++i) {
            checkCancel();
            OsmxWay way;
            way.id = static_cast<qint64>(cursor.u64());
            if (way.id == 0) {
                throw ParseFailure{OsmxErrorCode::CorruptRecord, QStringLiteral("道路 ID 不能为 0"),
                                   cursor.offset() - 8};
            }
            if (wayIds.contains(way.id)) {
                throw ParseFailure{OsmxErrorCode::DuplicateId, QStringLiteral("道路 ID 重复：%1").arg(way.id),
                                   cursor.offset() - 8};
            }
            const quint32 refCount = cursor.u32();
            if (refCount > static_cast<quint32>(cursor.remaining() / 8)) {
                throw ParseFailure{OsmxErrorCode::BadCount,
                                   QStringLiteral("道路 %1 的节点引用数量 %2 超出剩余字节允许范围")
                                       .arg(way.id)
                                       .arg(refCount),
                                   cursor.offset() - 4};
            }
            way.refFirst = static_cast<quint32>(data.wayNodes.size());
            way.refCount = refCount;
            for (quint32 r = 0; r < refCount; ++r) {
                const qint64 nodeId = static_cast<qint64>(cursor.u64());
                const auto found = nodeIds.constFind(nodeId);
                if (found == nodeIds.constEnd()) {
                    throw ParseFailure{OsmxErrorCode::CorruptRecord,
                                       QStringLiteral("道路 %1 引用了不存在的节点 %2").arg(way.id).arg(nodeId),
                                       cursor.offset() - 8};
                }
                data.wayNodes.append(found.value());
            }
            wayNodeReferences += refCount;
            if (refCount < 2) ++shortWays;
            quint32 tagFirst = 0;
            quint32 tagCount = 0;
            readTags(cursor, strings, data.wayTags, tagFirst, tagCount, checkCancel);
            way.tagFirst = tagFirst;
            way.tagCount = tagCount;
            wayTagCount += tagCount;
            fillAttributes(data.wayTags, tagFirst, tagCount, strings.values(), refCount, way.attributes);
            if (way.attributes.isRoutable()) ++routableWayCount;
            wayIds.insert(way.id);
            data.ways.append(way);
            if ((i & 0xFFu) == 0u || i + 1 == wayCount) {
                reportProgress(callbacks, lastPercent, lastStage,
                               60 + static_cast<int>(i * 22 / wayCount), QStringLiteral("解析道路"));
            }
        }
        data.info.wayTagCount = wayTagCount;
        data.info.wayNodeReferenceCount = wayNodeReferences;
        reportProgress(callbacks, lastPercent, lastStage, 82, QStringLiteral("解析关系"));

        const quint64 relationCount = cursor.u64();
        if (relationCount > static_cast<quint64>(cursor.remaining() / 16)) {
            throw ParseFailure{OsmxErrorCode::BadCount,
                               QStringLiteral("关系数量 %1 超出剩余字节允许范围").arg(relationCount),
                               cursor.offset() - 8};
        }
        if (relationCount > std::numeric_limits<quint32>::max()) {
            throw ParseFailure{OsmxErrorCode::BadCount, QStringLiteral("关系数量超过 32 位索引上限"),
                               cursor.offset() - 8};
        }
        data.relations.reserve(static_cast<qsizetype>(relationCount));
        QHash<qint64, quint32> relationIds;
        relationIds.reserve(static_cast<qsizetype>(relationCount));
        quint64 relationTagCount = 0;
        quint64 memberCount = 0;
        for (quint64 i = 0; i < relationCount; ++i) {
            if ((i & 0x3Fu) == 0u) checkCancel();
            OsmxRelation relation;
            relation.id = static_cast<qint64>(cursor.u64());
            if (relation.id == 0) {
                throw ParseFailure{OsmxErrorCode::CorruptRecord, QStringLiteral("关系 ID 不能为 0"),
                                   cursor.offset() - 8};
            }
            if (relationIds.contains(relation.id)) {
                throw ParseFailure{OsmxErrorCode::DuplicateId,
                                   QStringLiteral("关系 ID 重复：%1").arg(relation.id), cursor.offset() - 8};
            }
            const quint32 memberTotal = cursor.u32();
            if (memberTotal > static_cast<quint32>(cursor.remaining() / 16)) {
                throw ParseFailure{OsmxErrorCode::BadCount,
                                   QStringLiteral("关系 %1 的成员数量 %2 超出剩余字节允许范围")
                                       .arg(relation.id)
                                       .arg(memberTotal),
                                   cursor.offset() - 4};
            }
            relation.memberFirst = static_cast<quint32>(data.members.size());
            relation.memberCount = memberTotal;
            for (quint32 m = 0; m < memberTotal; ++m) {
                const quint32 typeValue = cursor.u32();
                if (typeValue > 2u) {
                    throw ParseFailure{OsmxErrorCode::CorruptRecord,
                                       QStringLiteral("关系 %1 的成员类型 %2 无效").arg(relation.id).arg(typeValue),
                                       cursor.offset() - 4};
                }
                const qint64 ref = static_cast<qint64>(cursor.u64());
                if (ref == 0) {
                    throw ParseFailure{OsmxErrorCode::CorruptRecord,
                                       QStringLiteral("关系 %1 的成员 ID 不能为 0").arg(relation.id),
                                       cursor.offset() - 8};
                }
                const QString role = readString(cursor);
                data.members.append(OsmxMember{static_cast<OsmxMemberType>(typeValue), ref,
                                               strings.intern(role)});
            }
            memberCount += memberTotal;
            quint32 tagFirst = 0;
            quint32 tagCount = 0;
            readTags(cursor, strings, data.relationTags, tagFirst, tagCount, checkCancel);
            relation.tagFirst = tagFirst;
            relation.tagCount = tagCount;
            relationTagCount += tagCount;
            relationIds.insert(relation.id, static_cast<quint32>(data.relations.size()));
            data.relations.append(relation);
            if ((i & 0x3Fu) == 0u || i + 1 == relationCount) {
                reportProgress(callbacks, lastPercent, lastStage,
                               82 + static_cast<int>(i * 10 / relationCount), QStringLiteral("解析关系"));
            }
        }
        data.info.relationTagCount = relationTagCount;
        data.info.relationMemberCount = memberCount;

        if (options.rejectTrailingBytes && cursor.remaining() != 0) {
            throw ParseFailure{OsmxErrorCode::TrailingData,
                               QStringLiteral("关系表之后仍有 %1 字节未识别数据").arg(cursor.remaining()),
                               cursor.offset()};
        }

        // Relation members are allowed to point outside the cleaned subset (a
        // cropped Wuhan extract still carries route relations); report them as
        // warnings instead of rejecting the import.
        quint64 missingNodeMembers = 0;
        quint64 missingWayMembers = 0;
        quint64 missingRelationMembers = 0;
        for (const auto& member : data.members) {
            switch (member.type) {
            case OsmxMemberType::Node:
                if (!nodeIds.contains(member.ref)) ++missingNodeMembers;
                break;
            case OsmxMemberType::Way:
                if (!wayIds.contains(member.ref)) ++missingWayMembers;
                break;
            case OsmxMemberType::Relation:
                if (!relationIds.contains(member.ref)) ++missingRelationMembers;
                break;
            }
        }
        QStringList warnings;
        if (missingNodeMembers + missingWayMembers + missingRelationMembers > 0) {
            warnings.append(QStringLiteral("关系成员引用缺失：节点 %1、道路 %2、关系 %3 个；原始成员已保留，"
                                           "依赖该关系的导航功能可能不完整。")
                                .arg(missingNodeMembers)
                                .arg(missingWayMembers)
                                .arg(missingRelationMembers));
        }
        if (shortWays > 0) {
            warnings.append(QStringLiteral("几何不足的道路 %1 条（节点引用少于 2 个），已排除出拓扑与导航图。")
                                .arg(shortWays));
        }
        if (routableWayCount == 0) {
            warnings.append(QStringLiteral("数据集没有可导航道路（highway 标签缺失或全部被排除），"
                                           "仅供渲染与查询，不能用于路径规划。"));
        }
        if (header.bounds.valid && computedBounds.valid) {
            const double tolerance = 1e-4;
            if (std::abs(header.bounds.minLat - computedBounds.minLat) > tolerance ||
                std::abs(header.bounds.minLon - computedBounds.minLon) > tolerance ||
                std::abs(header.bounds.maxLat - computedBounds.maxLat) > tolerance ||
                std::abs(header.bounds.maxLon - computedBounds.maxLon) > tolerance) {
                warnings.append(QStringLiteral("头部声明边界与节点实际范围不一致，已按实际节点重建边界。"));
            }
        }
        if (!computedBounds.valid) {
            computedBounds = header.bounds;
        }
        data.info.bounds = computedBounds;
        data.info.routableWayCount = routableWayCount;
        data.info.importedAt = options.importedAt.isEmpty()
                                   ? QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
                                   : options.importedAt;
        data.strings = strings.values();
        data.info.warnings = warnings;

        reportProgress(callbacks, lastPercent, lastStage, 92, QStringLiteral("构建拓扑与空间索引"));
        OsmxError buildError;
        result.dataset = OsmxDataset::create(std::move(data), &buildError);
        if (!result.dataset) {
            result.error = buildError;
            return result;
        }
        result.warnings = warnings;
        result.error.clear();
        reportProgress(callbacks, lastPercent, lastStage, 100, QStringLiteral("加载完成"));
        if (callbacks.log) {
            callbacks.log(QStringLiteral("OSMX v1：节点 %1、道路 %2（可导航 %3）、关系 %4、有向边 %5。")
                              .arg(result.dataset->nodeCount())
                              .arg(result.dataset->wayCount())
                              .arg(result.dataset->info().routableWayCount)
                              .arg(result.dataset->relationCount())
                              .arg(result.dataset->info().directedEdgeCount));
        }
    } catch (const Cancelled&) {
        result.cancelled = true;
        result.error.code = OsmxErrorCode::Cancelled;
        result.error.message = QStringLiteral("加载已取消，未发布数据集。");
    } catch (const ParseFailure& failure) {
        result.error.code = failure.code;
        result.error.message = failure.message;
        result.error.offset = failure.offset;
        result.dataset.reset();
    } catch (const std::exception& error) {
        result.error.code = OsmxErrorCode::InvalidDataset;
        result.error.message = QString::fromUtf8(error.what());
        result.dataset.reset();
    }

    const QFileInfo after(path);
    if (result.dataset && (after.size() != before.size() || after.lastModified() != before.lastModified())) {
        result.dataset.reset();
        result.error.code = OsmxErrorCode::FileChanged;
        result.error.message = QStringLiteral("加载期间源文件被修改，未发布数据集。");
    }
    result.elapsedMs = timer.elapsed();
    return result;
}

} // namespace p1::data
