#include "MapDemoWidget.h"

#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QBrush>
#include <QFont>

MapDemoWidget::MapDemoWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(640, 420);
    setWindowTitle(QStringLiteral("地图演示"));

    m_routePoints = {
        QPointF(100, 320),
        QPointF(190, 260),
        QPointF(280, 280),
        QPointF(370, 180),
        QPointF(500, 130)
    };
}

void MapDemoWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    painter.fillRect(rect(), QColor(238, 242, 234));

    QPen roadPen(QColor(255, 255, 255), 30,
                 Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(roadPen);

    painter.drawLine(QPointF(0, 100), QPointF(width(), 240));
    painter.drawLine(QPointF(120, 0), QPointF(360, height()));
    painter.drawLine(QPointF(width(), 60), QPointF(220, height()));

    if (m_routePoints.size() >= 2) {
        QPen routePen(QColor(35, 125, 235), 7,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(routePen);
        painter.drawPolyline(m_routePoints.constData(),
                             m_routePoints.size());
    }

    if (!m_routePoints.isEmpty()) {
        painter.setPen(QPen(Qt::white, 3));
        painter.setBrush(QColor(35, 180, 100));
        painter.drawEllipse(m_routePoints.first(), 10, 10);
    }

    if (m_routePoints.size() >= 2) {
        painter.setPen(QPen(Qt::white, 3));
        painter.setBrush(QColor(235, 75, 65));
        painter.drawEllipse(m_routePoints.last(), 10, 10);
    }

    painter.setPen(QColor(45, 55, 65));
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 14, QFont::Bold));
    painter.drawText(QPointF(20, 35),
                     QStringLiteral("固定场景 - 地图导航演示"));

    painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 10));
    painter.drawText(QPointF(20, height() - 20),
                     QStringLiteral("绿色：起点    蓝色：导航路线    红色：终点"));
}