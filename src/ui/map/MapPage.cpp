#include "ui/map/MapPage.h"

#include "contracts/NavigationTypes.h"
#include "pal/linux/renderer/LinuxRenderer.h"
#include "pal/linux/renderer/MockMapScene.h"
#include "ui/common/Components.h"
#include "ui/common/Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <optional>

namespace p1::ui {
namespace {

class MapCanvas final : public QWidget {
    Q_OBJECT
public:
    explicit MapCanvas(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName("mapRenderCanvas");
        setMinimumSize(560, 360);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setFrame(const QImage& frame) {
        frame_ = frame;
        setProperty("hasFrame", true);
        update();
    }

    QSize renderSize() const {
        return frame_.isNull() ? size().boundedTo(QSize(1440, 900)) : frame_.size();
    }

signals:
    void renderSizeChanged(QSize size);

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.fillRect(rect(), QColor(QStringLiteral("#EEF2EA")));
        if (frame_.isNull()) {
            painter.setPen(QColor(QStringLiteral("#625C73")));
            painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("正在准备地图渲染…"));
            return;
        }
        painter.drawImage(rect(), frame_);
    }

    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        emit renderSizeChanged(event->size());
    }

private:
    QImage frame_;
};

class MapPage final : public QWidget {
    Q_OBJECT
public:
    explicit MapPage(Theme& theme, QWidget* parent = nullptr)
        : QWidget(parent),
          theme_(theme),
          renderer_(this),
          scene_(renderer::makeMockMapScene()),
          navigation_(renderer::makeMockNavigationSnapshot()) {
        renderer_.setMode(renderer::RenderMode::CachedBase);
        setObjectName("mapPage");
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);

        auto* toolbar = new QHBoxLayout;
        toolbar->setSpacing(12);
        toolbar->addWidget(badge("演示场景 · Mock", "info"));
        toolbar->addWidget(label("后台渲染 · 非阻塞界面", "caption"));
        toolbar->addStretch();

        stats_ = label("等待首帧", "caption");
        stats_->setObjectName("mapRenderStats");
        stats_->setWordWrap(false);
        toolbar->addWidget(stats_);
        layout->addLayout(toolbar);

        auto* mapFrame = new QFrame;
        mapFrame->setProperty("flat", true);
        auto* mapLayout = new QVBoxLayout(mapFrame);
        mapLayout->setContentsMargins(8, 8, 8, 8);
        canvas_ = new MapCanvas;
        mapLayout->addWidget(canvas_);
        layout->addWidget(mapFrame, 1);

        connect(canvas_, &MapCanvas::renderSizeChanged, this, [this] {
            scheduleRender();
        });
        connect(&renderer_, &renderer::LinuxRenderer::frameReady,
                this, [this](const QImage& frame, const renderer::RenderStats& stats) {
                    canvas_->setFrame(frame);
                    stats_->setText(
                        QStringLiteral("%1 ms · %2 瓦片 / %3 路线 / %4 标记")
                            .arg(stats.elapsedUs / 1000.0, 0, 'f', 1)
                            .arg(stats.tileCount)
                            .arg(stats.routeCount)
                            .arg(stats.markerCount));
                });
        connect(&renderer_, &renderer::LinuxRenderer::errorOccurred,
                this, [this](const QString& error) {
                    stats_->setText(QStringLiteral("渲染失败：%1").arg(error));
                });

        QTimer::singleShot(0, this, &MapPage::scheduleRender);
    }

private:
    void scheduleRender() {
        QTimer::singleShot(0, this, [this] {
            const QSize size = canvas_->size();
            if (size.width() < 2 || size.height() < 2) return;
            renderer_.requestRender(scene_, navigation_, size);
        });
    }

    Theme& theme_;
    renderer::LinuxRenderer renderer_;
    renderer::MapScene scene_;
    std::optional<contracts::NavigationSnapshot> navigation_;
    QLabel* stats_ = nullptr;
    MapCanvas* canvas_ = nullptr;
};

} // namespace

QWidget* createMapPage(Theme& theme, QWidget* parent) {
    return new MapPage(theme, parent);
}

} // namespace p1::ui

#include "MapPage.moc"
