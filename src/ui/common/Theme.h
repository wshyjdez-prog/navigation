#pragma once

#include <QColor>
#include <QIcon>
#include <QObject>

class QApplication;

namespace p1::ui {
struct Colors {
    QString canvas, surface, raised, hover, selected;
    QString primary, primaryHover, primaryPressed, onPrimary;
    QString text, secondary, placeholder, disabled, border, control, focus;
    QString info, infoBg, success, successBg, warning, warningBg, error, errorBg;
};
struct MapColors {
    QColor land, road, roadEdge, majorRoad, label, route, track;
    QColor landuse, green, water, building, buildingEdge;
    QColor waterLabel, labelHalo; // area names: water text, halo behind all area names
    QColor poi, poiSelected;      // POI search pins and their name labels
};
// Cartographic colors are independent of the purple application chrome.
MapColors mapColors(bool dark);

// One application-owned theme. Changing it updates existing widgets in place.
class Theme final : public QObject {
    Q_OBJECT
public:
    explicit Theme(QObject* parent = nullptr);
    bool isDark() const { return dark_; }
    const Colors& colors() const { return colors_; }
    void apply(QApplication& app);
    void setDark(bool dark);
    QIcon icon(const QString& name) const;
signals:
    void changed();
private:
    bool dark_ = false;
    Colors colors_;
};
}
