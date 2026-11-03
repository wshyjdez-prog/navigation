#pragma once

#include <QWidget>
namespace p1::app { class NavigationWorkflow; }

namespace p1::ui {

class Theme;
QWidget* createMapPage(Theme& theme, QWidget* parent = nullptr);
QWidget* createMapPage(Theme&, app::NavigationWorkflow*, const QString& cacheDirectory, QWidget* parent=nullptr);
QWidget* createAlgorithmsPage(Theme&, app::NavigationWorkflow*, const QString& cacheDirectory, QWidget* parent=nullptr);

} // namespace p1::ui
