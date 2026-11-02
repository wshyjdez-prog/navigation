#pragma once

#include <QWidget>

namespace p1::ui {

class Theme;
QWidget* createMapPage(Theme& theme, QWidget* parent = nullptr);

} // namespace p1::ui
