#include "app/OsmxParser.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QString>
#include <QStringList>
#include <QtEndian>

#include <cstring>
#include <vector>

namespace p1::app {
namespace {

// The container is read through a bounded cursor: every field access checks
// the remaining bytes first, so a truncated or corrupted file produces an
// error message with an offset instead of a crash or a silent short read.
constexpr qint64 kMaxFileBytes = 512LL * 1024 * 1024;
constexpr quint32 kMaxStringBytes = 1u << 20; // 1 MiB per UTF-16 string
constexpr quint32 kMaxTagCount = 200000u;
constexpr quint32 kMaxWayReferences = 2000000u;
constexpr quint32 kMaxRelationMembers = 1000000u;

QString countText(quint64 value) {
    return QLocale().toString(static_cast<qulonglong>(value));
}

QString sizeText(qint64 bytes) {
    constexpr double kb = 1024.0;
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QStringLiteral("%1 KB").arg(QString::number(bytes / kb, 'f', 1));
    if (bytes < 1024LL * 1024 * 1024) return QStringLiteral("%1 MB").arg(QString::number(bytes / (kb * kb), 'f', 1));
    return QStringLiteral("%1 GB").arg(QString::number(bytes / (kb * kb * kb), 'f', 2));
}

// Little-endian reader over the loaded file image. The whole file is loaded
// once because QDataStream/QFile would perform one system call per field for
// millions of records; a 49 MiB fixture stays well below kMaxFileBytes.
class Cursor final {
public:
    explicit Cursor(const QByteArray& data) : data_(data) {}

    qint64 position() const { return position_; }
    qint64 size() const { return static_cast<qint64>(data_.size()); }
    qint64 remaining() const { return size() - position_; }
    bool atEnd() const { return position_ >= size(); }
    bool canRead(qint64 count) const { return count >= 0 && count <= remaining(); }

    bool skip(qint64 count) {
        if (!canRead(count)) return false;
        position_ += count;
        return true;
    }

    bool bytes(char* destination, qint64 count) {
        if (destination == nullptr || !canRead(count)) return false;
        std::memcpy(destination, data_.constData() + position_, static_cast<size_t>(count));
        position_ += count;
        return true;
    }

    bool u8(quint8& value) {
        if (!canRead(1)) return false;
        value = static_cast<quint8>(data_.constData()[position_]);
        ++position_;
        return true;
    }

    bool u32(quint32& value) {
        if (!canRead(4)) return false;
        value = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(data_.constData() + position_));
        position_ += 4;
        return true;
    }

    bool u64(quint64& value) {
        if (!canRead(8)) return false;
        value = qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(data_.constData() + position_));
        position_ += 8;
        return true;
    }

    bool f64(double& value) {
        quint64 bits = 0;
        if (!u64(bits)) return false;
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    }

private:
    const QByteArray& data_;
    qint64 position_ = 0;
};

bool readText(Cursor& cursor, quint32 byteLength, bool allowEmpty, QString& text, QString& error) {
    if (byteLength == 0) {
        if (!allowEmpty) {
            error = QStringLiteral("字符串长度为 0");
            return false;
        }
        text.clear();
        return true;
    }
    if (byteLength % 2 != 0 || byteLength > kMaxStringBytes) {
        error = QStringLiteral("UTF-16 字符串字节长度 %1 非法").arg(byteLength);
        return false;
    }
    if (!cursor.canRead(byteLength)) {
        error = QStringLiteral("字符串长度 %1 超出文件剩余空间").arg(byteLength);
        return false;
    }
    std::vector<char16_t> buffer(byteLength / 2);
    if (!cursor.bytes(reinterpret_cast<char*>(buffer.data()), byteLength)) {
        error = QStringLiteral("读取字符串失败");
        return false;
    }
    text = QString::fromUtf16(buffer.data(), static_cast<qsizetype>(buffer.size()));
    return true;
}

bool skipText(Cursor& cursor, quint32 byteLength, bool allowEmpty, QString& error) {
    if (byteLength == 0) {
        if (!allowEmpty) {
            error = QStringLiteral("字符串长度为 0");
            return false;
        }
        return true;
    }
    if (byteLength % 2 != 0 || byteLength > kMaxStringBytes) {
        error = QStringLiteral("UTF-16 字符串字节长度 %1 非法").arg(byteLength);
        return false;
    }
    if (!cursor.skip(byteLength)) {
        error = QStringLiteral("字符串长度 %1 超出文件剩余空间").arg(byteLength);
        return false;
    }
    return true;
}

} // namespace

ImportResult OsmxParser::parse(const QString& path,
                               const ImportProgressCallback& progress,
                               const ImportLogCallback& log,
                               const ImportCancelCallback& cancelled) const {
    QElapsedTimer timer;
    timer.start();

    ImportResult result;
    result.filePath = path;
    result.fileName = QFileInfo(path).fileName();

    auto writeLog = [&log](const QString& line) {
        if (log) log(line);
    };
    auto fail = [&result, &timer, &writeLog](const QString& message) {
        result.ok = false;
        result.error = message;
        result.elapsedMs = timer.elapsed();
        writeLog(QStringLiteral("导入失败：") + message);
        return result;
    };

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("无法打开文件：%1").arg(file.errorString()));
    }
    result.fileSize = file.size();
    if (result.fileSize <= 0) {
        return fail(QStringLiteral("文件为空，没有可导入的数据"));
    }
    if (result.fileSize > kMaxFileBytes) {
        return fail(QStringLiteral("文件过大（%1），当前版本最多支持 %2")
                        .arg(sizeText(result.fileSize))
                        .arg(sizeText(kMaxFileBytes)));
    }

    if (progress) progress(0, QStringLiteral("读取文件"));
    writeLog(QStringLiteral("读取文件 %1（%2）").arg(result.fileName).arg(sizeText(result.fileSize)));

    const QByteArray data = file.readAll();
    if (static_cast<qint64>(data.size()) != result.fileSize) {
        return fail(QStringLiteral("读取文件内容不完整（预期 %1 字节，实际 %2 字节）")
                        .arg(result.fileSize)
                        .arg(data.size()));
    }
    file.close();

    Cursor cursor(data);
    const qint64 total = cursor.size();
    int lastPercent = -1;
    auto report = [&cursor, &progress, total, &lastPercent](const QString& stage, bool force) {
        if (!progress || total <= 0) return;
        int percent = static_cast<int>((cursor.position() * 100) / total);
        percent = qBound(0, percent, 100);
        if (!force && percent == lastPercent) return;
        lastPercent = percent;
        progress(percent, stage);
    };
    auto cancelRequested = [&result, &timer, &cancelled, &writeLog]() {
        if (!cancelled || !cancelled()) return false;
        result.ok = false;
        result.cancelled = true;
        result.error = QStringLiteral("导入已取消");
        result.elapsedMs = timer.elapsed();
        writeLog(QStringLiteral("收到取消请求，已停止解析"));
        return true;
    };

    QString error;

    char magic[4] = {};
    if (!cursor.bytes(magic, 4)) {
        return fail(QStringLiteral("文件过短，缺少 OSMX 文件头"));
    }
    if (std::memcmp(magic, "OSMX", 4) != 0) {
        return fail(QStringLiteral("不是有效的 OSMX 文件：文件头标识不是 \"OSMX\""));
    }

    quint32 version = 0;
    if (!cursor.u32(version)) {
        return fail(QStringLiteral("文件头不完整：缺少格式版本"));
    }
    if (version != 1) {
        return fail(QStringLiteral("不支持的 OSMX 版本 %1（当前仅支持版本 1）").arg(version));
    }
    result.formatVersion = version;

    quint32 sourceLength = 0;
    if (!cursor.u32(sourceLength)) {
        return fail(QStringLiteral("文件头不完整：缺少来源名称长度"));
    }
    if (!readText(cursor, sourceLength, true, result.sourceName, error)) {
        return fail(QStringLiteral("文件头来源名称无效：%1").arg(error));
    }
    writeLog(QStringLiteral("文件头校验通过：OSMX v%1").arg(version));
    if (!result.sourceName.isEmpty()) {
        writeLog(QStringLiteral("原始来源：%1").arg(result.sourceName));
    }

    quint8 flags = 0;
    if (!cursor.u8(flags)) {
        return fail(QStringLiteral("文件头不完整：缺少数据标志"));
    }
    if (flags > 1) {
        return fail(QStringLiteral("未知的数据标志 %1（当前仅支持 0 或 1）").arg(flags));
    }
    if (flags == 1) {
        if (!cursor.f64(result.minLat) || !cursor.f64(result.minLon) ||
            !cursor.f64(result.maxLat) || !cursor.f64(result.maxLon)) {
            return fail(QStringLiteral("文件头不完整：缺少覆盖范围"));
        }
        if (result.minLat < -90.0 || result.minLat > 90.0 ||
            result.maxLat < -90.0 || result.maxLat > 90.0 ||
            result.minLon < -180.0 || result.minLon > 180.0 ||
            result.maxLon < -180.0 || result.maxLon > 180.0) {
            return fail(QStringLiteral("覆盖范围含非法经纬度"));
        }
        if (result.minLat > result.maxLat || result.minLon > result.maxLon) {
            return fail(QStringLiteral("覆盖范围的最小值大于最大值"));
        }
        result.hasBounds = true;
        writeLog(QStringLiteral("覆盖范围：%1, %2 — %3, %4")
                     .arg(result.minLat, 0, 'f', 6)
                     .arg(result.minLon, 0, 'f', 6)
                     .arg(result.maxLat, 0, 'f', 6)
                     .arg(result.maxLon, 0, 'f', 6));
    }

    quint64 nodeCount = 0;
    if (!cursor.u64(nodeCount)) {
        return fail(QStringLiteral("文件头不完整：缺少节点数量"));
    }
    // A node record occupies at least 28 bytes, so a count that cannot fit is
    // a corrupted header rather than a valid dataset.
    if (nodeCount > static_cast<quint64>(cursor.remaining() / 28)) {
        return fail(QStringLiteral("节点数量 %1 超出文件剩余空间能容纳的记录数").arg(countText(nodeCount)));
    }
    writeLog(QStringLiteral("节点数量：%1").arg(countText(nodeCount)));

    QStringList previewTags;
    quint64 tagCounter = 0;
    auto readTags = [&](quint32 tagCount) -> bool {
        if (tagCount > kMaxTagCount) {
            error = QStringLiteral("标签数量 %1 超出上限 %2").arg(tagCount).arg(kMaxTagCount);
            return false;
        }
        for (quint32 t = 0; t < tagCount; ++t) {
            quint32 keyLength = 0;
            if (!cursor.u32(keyLength)) {
                error = QStringLiteral("标签键长度缺失（文件被截断）");
                return false;
            }
            if (keyLength == 0 || keyLength % 2 != 0 || keyLength > kMaxStringBytes) {
                error = QStringLiteral("标签键字节长度 %1 非法").arg(keyLength);
                return false;
            }
            const bool capture = previewTags.size() < 6;
            QString key;
            if (capture) {
                if (!readText(cursor, keyLength, false, key, error)) return false;
            } else if (!skipText(cursor, keyLength, false, error)) {
                return false;
            }

            quint32 valueLength = 0;
            if (!cursor.u32(valueLength)) {
                error = QStringLiteral("标签值长度缺失（文件被截断）");
                return false;
            }
            if (valueLength % 2 != 0 || valueLength > kMaxStringBytes) {
                error = QStringLiteral("标签值字节长度 %1 非法").arg(valueLength);
                return false;
            }
            if (capture) {
                QString value;
                if (!readText(cursor, valueLength, true, value, error)) return false;
                previewTags << (value.isEmpty() ? key : key + QLatin1Char('=') + value);
            } else if (!skipText(cursor, valueLength, true, error)) {
                return false;
            }
            ++tagCounter;
        }
        return true;
    };

    report(QStringLiteral("解析节点"), true);
    writeLog(QStringLiteral("开始解析节点…"));
    for (quint64 i = 0; i < nodeCount; ++i) {
        if ((i & 0x7FF) == 0) {
            if (cancelRequested()) return result;
            report(QStringLiteral("解析节点"), false);
        }
        quint64 id = 0;
        double lat = 0.0;
        double lon = 0.0;
        quint32 tagCount = 0;
        if (!cursor.u64(id) || !cursor.f64(lat) || !cursor.f64(lon) || !cursor.u32(tagCount)) {
            return fail(QStringLiteral("节点 #%1 记录被截断（偏移 %2）").arg(i).arg(cursor.position()));
        }
        if (id == 0) {
            return fail(QStringLiteral("节点 #%1 的 ID 为 0（偏移 %2）").arg(i).arg(cursor.position()));
        }
        if (!(lat >= -90.0 && lat <= 90.0) || !(lon >= -180.0 && lon <= 180.0)) {
            return fail(QStringLiteral("节点 #%1（ID %2）坐标越界：%3, %4")
                            .arg(i)
                            .arg(id)
                            .arg(lat, 0, 'f', 6)
                            .arg(lon, 0, 'f', 6));
        }
        if (!readTags(tagCount)) {
            return fail(QStringLiteral("节点 #%1：%2（偏移 %3）").arg(i).arg(error).arg(cursor.position()));
        }
        ++result.stats.nodes;
    }
    result.stats.tags += tagCounter;
    tagCounter = 0;
    if (cancelRequested()) return result;
    report(QStringLiteral("解析节点"), true);
    writeLog(QStringLiteral("节点解析完成：%1 个").arg(countText(result.stats.nodes)));

    quint64 wayCount = 0;
    if (!cursor.u64(wayCount)) {
        return fail(QStringLiteral("节点数据结束后缺少道路数量"));
    }
    if (wayCount > static_cast<quint64>(cursor.remaining() / 12)) {
        return fail(QStringLiteral("道路数量 %1 超出文件剩余空间能容纳的记录数").arg(countText(wayCount)));
    }
    writeLog(QStringLiteral("道路数量：%1").arg(countText(wayCount)));

    report(QStringLiteral("解析道路"), true);
    for (quint64 i = 0; i < wayCount; ++i) {
        if ((i & 0x1FF) == 0) {
            if (cancelRequested()) return result;
            report(QStringLiteral("解析道路"), false);
        }
        quint64 id = 0;
        quint32 refCount = 0;
        if (!cursor.u64(id) || !cursor.u32(refCount)) {
            return fail(QStringLiteral("道路 #%1 记录被截断（偏移 %2）").arg(i).arg(cursor.position()));
        }
        if (id == 0) {
            return fail(QStringLiteral("道路 #%1 的 ID 为 0（偏移 %2）").arg(i).arg(cursor.position()));
        }
        if (refCount > kMaxWayReferences) {
            return fail(QStringLiteral("道路 #%1 的节点引用数量 %2 超出上限").arg(i).arg(refCount));
        }
        const qint64 referenceBytes = static_cast<qint64>(refCount) * 8;
        if (!cursor.canRead(referenceBytes)) {
            return fail(QStringLiteral("道路 #%1 的节点引用超出文件剩余空间（偏移 %2）").arg(i).arg(cursor.position()));
        }
        if (!cursor.skip(referenceBytes)) {
            return fail(QStringLiteral("道路 #%1 的节点引用读取失败（偏移 %2）").arg(i).arg(cursor.position()));
        }
        quint32 tagCount = 0;
        if (!cursor.u32(tagCount)) {
            return fail(QStringLiteral("道路 #%1 缺少标签数量（偏移 %2）").arg(i).arg(cursor.position()));
        }
        if (!readTags(tagCount)) {
            return fail(QStringLiteral("道路 #%1：%2（偏移 %3）").arg(i).arg(error).arg(cursor.position()));
        }
        ++result.stats.ways;
        result.stats.nodeReferences += refCount;
    }
    result.stats.tags += tagCounter;
    tagCounter = 0;
    if (cancelRequested()) return result;
    report(QStringLiteral("解析道路"), true);
    writeLog(QStringLiteral("道路解析完成：%1 条，节点引用 %2 个")
                 .arg(countText(result.stats.ways))
                 .arg(countText(result.stats.nodeReferences)));

    quint64 relationCount = 0;
    if (!cursor.u64(relationCount)) {
        return fail(QStringLiteral("道路数据结束后缺少关系数量"));
    }
    if (relationCount > static_cast<quint64>(cursor.remaining() / 12)) {
        return fail(QStringLiteral("关系数量 %1 超出文件剩余空间能容纳的记录数").arg(countText(relationCount)));
    }
    writeLog(QStringLiteral("关系数量：%1").arg(countText(relationCount)));

    report(QStringLiteral("解析关系"), true);
    for (quint64 i = 0; i < relationCount; ++i) {
        if ((i & 0xFF) == 0) {
            if (cancelRequested()) return result;
            report(QStringLiteral("解析关系"), false);
        }
        quint64 id = 0;
        quint32 memberCount = 0;
        if (!cursor.u64(id) || !cursor.u32(memberCount)) {
            return fail(QStringLiteral("关系 #%1 记录被截断（偏移 %2）").arg(i).arg(cursor.position()));
        }
        if (id == 0) {
            return fail(QStringLiteral("关系 #%1 的 ID 为 0（偏移 %2）").arg(i).arg(cursor.position()));
        }
        if (memberCount > kMaxRelationMembers) {
            return fail(QStringLiteral("关系 #%1 的成员数量 %2 超出上限").arg(i).arg(memberCount));
        }
        for (quint32 m = 0; m < memberCount; ++m) {
            quint32 memberType = 0;
            quint64 reference = 0;
            quint32 roleLength = 0;
            if (!cursor.u32(memberType) || !cursor.u64(reference) || !cursor.u32(roleLength)) {
                return fail(QStringLiteral("关系 #%1 成员 #%2 记录被截断（偏移 %3）")
                                .arg(i)
                                .arg(m)
                                .arg(cursor.position()));
            }
            if (memberType > 2) {
                return fail(QStringLiteral("关系 #%1 成员 #%2 的类型 %3 非法").arg(i).arg(m).arg(memberType));
            }
            if (reference == 0) {
                return fail(QStringLiteral("关系 #%1 成员 #%2 的引用为 0（偏移 %3）").arg(i).arg(m).arg(cursor.position()));
            }
            // Members may legitimately have an empty role, e.g. multipolygons.
            if (!skipText(cursor, roleLength, true, error)) {
                return fail(QStringLiteral("关系 #%1 成员 #%2：%3（偏移 %4）")
                                .arg(i)
                                .arg(m)
                                .arg(error)
                                .arg(cursor.position()));
            }
        }
        quint32 tagCount = 0;
        if (!cursor.u32(tagCount)) {
            return fail(QStringLiteral("关系 #%1 缺少标签数量（偏移 %2）").arg(i).arg(cursor.position()));
        }
        if (!readTags(tagCount)) {
            return fail(QStringLiteral("关系 #%1：%2（偏移 %3）").arg(i).arg(error).arg(cursor.position()));
        }
        ++result.stats.relations;
        result.stats.relationMembers += memberCount;
    }
    result.stats.tags += tagCounter;
    if (cancelRequested()) return result;
    writeLog(QStringLiteral("关系解析完成：%1 个，成员 %2 个")
                 .arg(countText(result.stats.relations))
                 .arg(countText(result.stats.relationMembers)));

    report(QStringLiteral("校验结构"), true);
    if (!cursor.atEnd()) {
        return fail(QStringLiteral("文件在偏移 %1 处仍有 %2 字节未解析，结构可能不受支持")
                        .arg(cursor.position())
                        .arg(cursor.remaining()));
    }
    if (!previewTags.isEmpty()) {
        writeLog(QStringLiteral("示例标签：%1").arg(previewTags.join(QStringLiteral("、"))));
    }

    result.ok = true;
    result.elapsedMs = timer.elapsed();
    if (progress) progress(100, QStringLiteral("完成"));
    writeLog(QStringLiteral("结构校验完成：共读取 %1 字节").arg(cursor.position()));
    writeLog(QStringLiteral("导入完成：节点 %1，道路 %2，关系 %3，标签 %4，用时 %5 ms")
                 .arg(countText(result.stats.nodes))
                 .arg(countText(result.stats.ways))
                 .arg(countText(result.stats.relations))
                 .arg(countText(result.stats.tags))
                 .arg(result.elapsedMs));
    return result;
}

} // namespace p1::app
