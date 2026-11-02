#include "pal/linux/renderer/LinuxRenderer.h"
#include "pal/linux/renderer/MockMapScene.h"

#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QtMath>

#include <algorithm>
#include <numeric>

namespace {

p1::renderer::MapScene makeBenchmarkScene() {
    auto scene = p1::renderer::makeMockMapScene();
    auto& route = scene.routes.front();
    route.points.clear();
    route.points.reserve(1024);
    for (int index = 0; index < 1024; ++index) {
        const qreal x = 40.0 + index * (944.0 / 1023.0);
        const qreal y = 430.0 - 120.0 * qSin(index / 42.0) - index * 0.18;
        route.points.push_back(QPointF(x, y));
    }
    return scene;
}

qint64 percentile(QVector<qint64> values, double fraction) {
    std::sort(values.begin(), values.end());
    if (values.isEmpty()) return -1;
    const int lastIndex = static_cast<int>(values.size()) - 1;
    const int index = std::clamp(
        static_cast<int>(qCeil(values.size() * fraction)) - 1,
        0,
        lastIndex);
    return values[index];
}

qint64 peakRssKiB() {
#ifdef Q_OS_LINUX
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly)) return -1;
    while (!status.atEnd()) {
        const QByteArray line = status.readLine();
        if (line.startsWith("VmHWM:")) {
            const auto parts = line.simplified().split(' ');
            if (parts.size() >= 2) return parts[1].toLongLong();
        }
    }
#endif
    return -1;
}

QJsonObject measure(p1::renderer::RenderMode mode,
                    const p1::renderer::MapScene& scene,
                    const QSize& outputSize,
                    int iterations) {
    QVector<qint64> samples;
    samples.reserve(iterations);
    QImage cache;
    QString cacheKey;

    for (int index = 0; index < 5; ++index) {
        const auto warmup = p1::renderer::LinuxRenderer::render(
            scene, nullptr, outputSize, mode, 0, cache, cacheKey);
        cache = warmup.baseCache;
        cacheKey = warmup.baseCacheKey;
    }

    int cacheHits = 0;
    for (int index = 0; index < iterations; ++index) {
        const auto result = p1::renderer::LinuxRenderer::render(
            scene, nullptr, outputSize, mode, quint64(index + 1), cache, cacheKey);
        if (result.stats.cacheHit) ++cacheHits;
        samples.push_back(result.stats.elapsedUs);
        cache = result.baseCache;
        cacheKey = result.baseCacheKey;
    }

    const qint64 total = std::accumulate(
        samples.cbegin(), samples.cend(), qint64(0));
    QVariantList sampleValues;
    sampleValues.reserve(samples.size());
    for (const qint64 value : samples) sampleValues.push_back(value);

    QJsonObject object;
    object.insert("mode", QString::fromLatin1(p1::renderer::toString(mode)));
    object.insert("iterations", iterations);
    object.insert("samplesUs", QJsonArray::fromVariantList(sampleValues));
    object.insert("medianUs", static_cast<double>(percentile(samples, 0.50)));
    object.insert("p95Us", static_cast<double>(percentile(samples, 0.95)));
    object.insert("meanUs", double(total) / samples.size());
    object.insert("cacheHits", cacheHits);
    object.insert("peakRssKiB", static_cast<double>(peakRssKiB()));
    return object;
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const auto scene = makeBenchmarkScene();
    const QSize outputSize(1024, 640);

    QJsonObject root;
    root.insert("sceneVersion", scene.dataVersion);
    root.insert("qtVersion", QString::fromLatin1(QT_VERSION_STR));
#if defined(__GNUC__)
    root.insert(
        "compiler",
        QStringLiteral("GCC %1.%2.%3")
            .arg(__GNUC__)
            .arg(__GNUC_MINOR__)
            .arg(__GNUC_PATCHLEVEL__));
#elif defined(_MSC_VER)
    root.insert("compiler", QStringLiteral("MSVC %1").arg(_MSC_VER));
#else
    root.insert("compiler", QStringLiteral("unknown"));
#endif
#ifdef QT_NO_DEBUG
    root.insert("buildType", QStringLiteral("Release"));
#else
    root.insert("buildType", QStringLiteral("Debug"));
#endif
    root.insert(
        "outputSize",
        QStringLiteral("%1x%2").arg(outputSize.width()).arg(outputSize.height()));
    root.insert("tiles", scene.tiles.size());
    root.insert(
        "routePoints",
        scene.routes.isEmpty() ? 0 : scene.routes.front().points.size());
    root.insert("markers", scene.markers.size());
    root.insert("warmupIterations", 5);
    root.insert("iterations", 50);
    root.insert("cachedBase", measure(
        p1::renderer::RenderMode::CachedBase, scene, outputSize, 50));
    root.insert("immediate", measure(
        p1::renderer::RenderMode::Immediate, scene, outputSize, 50));

    QTextStream out(stdout);
    out << QJsonDocument(root).toJson(QJsonDocument::Indented);
    return 0;
}
