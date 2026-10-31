#pragma once

#include "app/ImportParser.h"

namespace p1::app {

// Reader for the binary OSMX container (magic "OSMX", format version 1).
//
// Layout, all integers little endian:
//   char[4]  "OSMX"
//   uint32   format version (1)
//   uint32   byte length of the source name
//   char16[] source name (UTF-16LE, length bytes, no terminator)
//   uint8    bounds flag (1 = bounds follow)
//   double   minLat, minLon, maxLat, maxLon      (only when the flag is 1)
//   uint64   node count
//   node...  uint64 id, double lat, double lon, uint32 tag count, tags
//   uint64   way count
//   way...   uint64 id, uint32 ref count, uint64 refs[], uint32 tag count, tags
//   uint64   relation count
//   relation uint64 id, uint32 member count,
//            member: uint32 type (0 node, 1 way, 2 relation), uint64 ref,
//                    uint32 role length, char16[] role
//            uint32 tag count, tags
//   tag      uint32 key length, char16[] key, uint32 value length, char16[] value
class OsmxParser final : public ImportParser {
public:
    QString name() const override { return QStringLiteral("OSMX v1"); }
    QStringList extensions() const override { return {QStringLiteral("osmx")}; }

    ImportResult parse(const QString& path,
                       const ImportProgressCallback& progress,
                       const ImportLogCallback& log,
                       const ImportCancelCallback& cancelled) const override;
};

} // namespace p1::app
