#pragma once

#include "pal/linux/renderer/MapScene.h"
#include "contracts/NavigationTypes.h"

#include <QFutureWatcher>
#include <QImage>
#include <QObject>
#include <QThreadPool>

#include <optional>

namespace p1::renderer {

enum class RenderMode {
    CachedBase,
    Immediate
};

struct RenderStats {
    quint64 requestId = 0;
    RenderMode mode = RenderMode::CachedBase;
    qint64 elapsedUs = 0;
    bool cacheHit = false;
    int tileCount = 0;
    int routeCount = 0;
    int markerCount = 0;
    QSize outputSize;
};

struct RenderResult {
    QImage frame;
    QImage baseCache;
    QString baseCacheKey;
    QString error;
    RenderStats stats;
};

const char* toString(RenderMode mode);

class LinuxRenderer final : public QObject {
    Q_OBJECT
public:
    explicit LinuxRenderer(QObject* parent = nullptr);
    ~LinuxRenderer() override;

    void setMode(RenderMode mode);
    RenderMode mode() const { return mode_; }

    // GUI thread only. Requests are coalesced: a running frame is allowed to finish,
    // but only the latest queued request is kept and later frames are emitted.
    void requestRender(const MapScene& scene,
                       const std::optional<contracts::NavigationSnapshot>& navigation,
                       const QSize& outputSize);

    // Pure rendering entry used by tests and the benchmark tool.
    static RenderResult render(const MapScene& scene,
                               const contracts::NavigationSnapshot* navigation,
                               const QSize& outputSize,
                               RenderMode mode,
                               quint64 requestId,
                               const QImage& cachedBase = {},
                               const QString& cachedKey = {});

signals:
    void frameReady(const QImage& frame, const p1::renderer::RenderStats& stats);
    void errorOccurred(const QString& error);

private:
    struct Request {
        quint64 id = 0;
        MapScene scene;
        std::optional<contracts::NavigationSnapshot> navigation;
        QSize outputSize;
    };

    void startRequest(Request request);
    void finishRequest();

    QThreadPool pool_;
    QFutureWatcher<RenderResult>* watcher_ = nullptr;
    std::optional<Request> pending_;
    QImage cachedBase_;
    QString cachedBaseKey_;
    RenderMode mode_ = RenderMode::CachedBase;
    quint64 nextRequestId_ = 0;
    quint64 latestRequestId_ = 0;
    bool busy_ = false;
};

} // namespace p1::renderer

Q_DECLARE_METATYPE(p1::renderer::RenderStats)
