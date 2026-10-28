#ifndef MAPDEMOWIDGET_H
#define MAPDEMOWIDGET_H

#include <QWidget>
#include <QPointF>
#include <QVector>

class QPainter;

class MapDemoWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MapDemoWidget(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVector<QPointF> m_routePoints;
};

#endif // MAPDEMOWIDGET_H