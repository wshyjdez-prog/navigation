#include "pal/linux/renderer/LinuxRenderer.h"
#include "pal/linux/renderer/MockMapScene.h"

#include <QElapsedTimer>
#include <QImage>
#include <QSet>
#include <QtTest>

#include <limits>

using p1::renderer::LinuxRenderer;
using p1::renderer::RenderMode;

class RendererTests final : public QObject {
    Q_OBJECT
private slots:
    void validSceneRendersAllLayers() {
        const auto scene = p1::renderer::makeMockMapScene();
        const auto navigation = p1::renderer::makeMockNavigationSnapshot();
        const auto result = LinuxRenderer::render(
            scene, &navigation, QSize(1024, 640), RenderMode::Immediate, 7);

        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(!result.frame.isNull());
        QCOMPARE(result.frame.size(), QSize(1024, 640));
        QCOMPARE(result.stats.requestId, quint64(7));
        QCOMPARE(result.stats.tileCount, scene.tiles.size());
        QCOMPARE(result.stats.routeCount, scene.routes.size());
        QCOMPARE(result.stats.markerCount, scene.markers.size());
        QVERIFY(!result.stats.cacheHit);

        QSet<QRgb> colors;
        for (int y = 0; y < result.frame.height(); y += 8) {
            for (int x = 0; x < result.frame.width(); x += 8) {
                colors.insert(result.frame.pixel(x, y));
            }
        }
        QVERIFY2(colors.size() > 8, "Rendered scene should contain map, route and marker colors");
    }

    void cachedModeHitsBaseWhileImmediateDoesNot() {
        const auto scene = p1::renderer::makeMockMapScene();
        const QSize output(1024, 640);

        const auto coldA = LinuxRenderer::render(
            scene, nullptr, output, RenderMode::CachedBase, 1);
        QVERIFY2(coldA.error.isEmpty(), qPrintable(coldA.error));
        QVERIFY(!coldA.stats.cacheHit);

        const auto warmA = LinuxRenderer::render(
            scene, nullptr, output, RenderMode::CachedBase, 2,
            coldA.baseCache, coldA.baseCacheKey);
        QVERIFY2(warmA.error.isEmpty(), qPrintable(warmA.error));
        QVERIFY(warmA.stats.cacheHit);

        const auto immediate = LinuxRenderer::render(
            scene, nullptr, output, RenderMode::Immediate, 3,
            coldA.baseCache, coldA.baseCacheKey);
        QVERIFY(!immediate.stats.cacheHit);
        QCOMPARE(immediate.frame, coldA.frame);
    }

    void cacheKeyChangesWithTileContent() {
        auto scene = p1::renderer::makeMockMapScene();
        const QSize output(1024, 640);
        const QString original = p1::renderer::baseCacheKey(scene, output);
        scene.tiles[0].id += QStringLiteral("-changed");
        QVERIFY(original != p1::renderer::baseCacheKey(scene, output));
    }

    void validationRejectsInvalidInputs() {
        const QSize output(1024, 640);
        auto scene = p1::renderer::makeMockMapScene();

        auto missingVersion = scene;
        missingVersion.dataVersion.clear();
        QVERIFY(!p1::renderer::validateScene(missingVersion, output).valid);

        auto missingImage = scene;
        missingImage.tiles[0].image = {};
        QVERIFY(!p1::renderer::validateScene(missingImage, output).valid);

        auto shortRoute = scene;
        shortRoute.routes[0].points = {QPointF(1, 2)};
        QVERIFY(!p1::renderer::validateScene(shortRoute, output).valid);

        auto nonFinite = scene;
        nonFinite.routes[0].points[1].setX(std::numeric_limits<qreal>::infinity());
        QVERIFY(!p1::renderer::validateScene(nonFinite, output).valid);
    }

    void navigationOverlayChangesFrame() {
        const auto scene = p1::renderer::makeMockMapScene();
        const auto navigation = p1::renderer::makeMockNavigationSnapshot();
        const QSize output(1024, 640);
        const auto plain = LinuxRenderer::render(
            scene, nullptr, output, RenderMode::Immediate, 1);
        const auto guided = LinuxRenderer::render(
            scene, &navigation, output, RenderMode::Immediate, 2);
        QVERIFY(plain.frame != guided.frame);
    }

    void latestAsyncRequestIsKept() {
        LinuxRenderer renderer;
        QSignalSpy errors(&renderer, &LinuxRenderer::errorOccurred);
        QVector<quint64> receivedIds;
        QObject::connect(
            &renderer,
            &LinuxRenderer::frameReady,
            this,
            [&receivedIds](const QImage&, const p1::renderer::RenderStats& stats) {
                receivedIds.push_back(stats.requestId);
            });

        const auto scene = p1::renderer::makeMockMapScene();
        QElapsedTimer submitTimer;
        submitTimer.start();
        renderer.requestRender(scene, std::nullopt, QSize(800, 500));
        renderer.requestRender(scene, std::nullopt, QSize(900, 560));
        QVERIFY2(submitTimer.elapsed() < 100,
                 "Asynchronous submission must not wait for a rendered frame");
        QTRY_VERIFY_WITH_TIMEOUT(!receivedIds.isEmpty(), 5000);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(receivedIds.back(), quint64(2));
    }
};

QTEST_MAIN(RendererTests)
#include "RendererTests.moc"
