#include "ui/common/Theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>

namespace p1::ui {
MapColors mapColors(bool dark) {
    return dark ? MapColors{"#242729","#596065","#353B3F","#737B80","#D4D9DD","#67A9FF","#A7B0B7","#303234","#293D32","#263F50","#42464A","#51575C",QColor("#9CCBEA"),QColor(36,39,41,220),QColor("#B09CDE"),QColor("#D7CAFC")}
                : MapColors{"#F2F3F4","#FFFFFF","#D2D5D8","#E1E4E7","#5C636A","#2879E8","#7C858E","#ECEAE5","#DAE8D2","#BBDCEB","#E0DDD8","#C8C5C0",QColor("#2A5B7C"),QColor(242,243,244,220),QColor("#4335BC"),QColor("#2E228E")};
}
Theme::Theme(QObject* parent) : QObject(parent) {}

void Theme::setDark(bool dark) {
    if (dark_ == dark) return;
    dark_ = dark;
    apply(*qApp);
    emit changed();
}

void Theme::apply(QApplication& app) {
    colors_ = dark_ ? Colors{
        "#191823", "#242232", "#302C41", "#3B3451", "#44385E",
        "#C6B5F4", "#D7CAFC", "#B09CDE", "#211A35",
        "#F4F0FA", "#C5BED2", "#B2A8C5", "#82798F", "#494056", "#A69AB9", "#D7CAFC",
        "#AFC5FF", "#293752", "#90DBB8", "#203E33", "#F0C17B", "#48351F", "#FFA2B5", "#4B2935"
    } : Colors{
        "#EDE7F0", "#F9F6FC", "#FFFFFF", "#E9E1F4", "#E2D8F3",
        "#4335BC", "#382BA6", "#2E228E", "#FFFFFF",
        "#202335", "#625C73", "#716A81", "#8F879C", "#DCD4E6", "#81778F", "#4335BC",
        "#345DB3", "#E9EFFC", "#21684F", "#E4F2EB", "#875016", "#FFF0DA", "#B32F49", "#FBE9EE"
    };
    const auto& c = colors_;
    const QStringList installedFonts = QFontDatabase::families();
    QStringList families;
    for (const QString& candidate : {
             QStringLiteral("Noto Sans CJK SC"),
             QStringLiteral("Microsoft YaHei"),
             QStringLiteral("Noto Sans"),
             QStringLiteral("DejaVu Sans")
         }) {
        if (installedFonts.contains(candidate, Qt::CaseInsensitive)) {
            families.push_back(candidate);
        }
    }
    QFont font;
    if (!families.isEmpty()) font.setFamilies(families);
    font.setStyleHint(QFont::SansSerif);
    font.setPixelSize(14);
    app.setFont(font);
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(c.canvas));
    palette.setColor(QPalette::WindowText, QColor(c.text));
    palette.setColor(QPalette::Base, QColor(c.raised));
    palette.setColor(QPalette::AlternateBase, QColor(c.surface));
    palette.setColor(QPalette::Text, QColor(c.text));
    palette.setColor(QPalette::Button, QColor(c.raised));
    palette.setColor(QPalette::ButtonText, QColor(c.text));
    palette.setColor(QPalette::Highlight, QColor(c.primary));
    palette.setColor(QPalette::HighlightedText, QColor(c.onPrimary));
    palette.setColor(QPalette::PlaceholderText, QColor(c.placeholder));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(c.disabled));
    app.setPalette(palette);

    QString qss = QString::fromUtf8(R"(
        QWidget { color: @text; }
        QMainWindow, QWidget#canvas { background: @canvas; }
        QLabel { background: transparent; border: none; }
        QLabel[role="pageTitle"] { font-size: 28px; font-weight: 700; }
        QLabel[role="sectionTitle"] { font-size: 20px; font-weight: 600; }
        QLabel[role="cardTitle"] { font-size: 16px; font-weight: 600; }
        QLabel[role="metric"] { font-size: 32px; font-weight: 700; }
        QLabel[role="secondary"], QLabel[role="caption"] { color: @secondary; }
        QLabel[role="caption"] { font-size: 12px; }
        QLabel[role="eyebrow"] { color: @primary; font-size: 12px; font-weight: 600; }
        QFrame[card="true"], QFrame#sidebar {
            background: @surface; border: 1px solid @border; border-radius: 20px;
        }
        QFrame[flat="true"] { background: @raised; border: 1px solid @border; border-radius: 12px; }
        QLabel[badge="info"] { color: @info; background: @infoBg; border-radius: 8px; padding: 5px 10px; font-size: 12px; }
        QLabel[badge="warning"] { color: @warning; background: @warningBg; border-radius: 8px; padding: 5px 10px; font-size: 12px; }
        QLabel[badge="success"] { color: @success; background: @successBg; border-radius: 8px; padding: 5px 10px; font-size: 12px; }
        QPushButton { background: @raised; border: 1px solid @control; border-radius: 12px;
            min-height: 42px; padding: 0 18px; font-weight: 500; }
        QPushButton:hover { background: @hover; }
        QPushButton:pressed { background: @selected; }
        QPushButton:focus { border: 2px solid @focus; }
        QPushButton[variant="primary"] { background: @primary; color: @onPrimary; border-color: @primary; }
        QPushButton[variant="primary"]:hover { background: @primaryHover; }
        QPushButton[variant="primary"]:pressed { background: @primaryPressed; }
        QPushButton:disabled { color: @disabled; background: @surface; border-color: @border; }
        QPushButton[nav="true"] { text-align: left; border: 3px solid transparent; min-height: 42px; padding: 0 12px; background: transparent; }
        QPushButton[nav="true"]:hover { background: @hover; }
        QPushButton[nav="true"]:checked { background: @selected; color: @primary; border-left-color: @primary; }
        QPushButton[nav="true"]:focus { border-top-color: @focus; border-bottom-color: @focus; border-right-color: @focus; }
        QPushButton[nav="true"]:disabled { color: @disabled; }
        QLineEdit, QComboBox, QSpinBox { background: @raised; color: @text; border: 1px solid @control;
            border-radius: 12px; min-height: 42px; padding: 0 14px; selection-background-color: @primary; selection-color: @onPrimary; }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border: 2px solid @focus; }
        QComboBox QAbstractItemView { background: @raised; color: @text; selection-background-color: @selected; selection-color: @text; }
        QProgressBar { border: none; border-radius: 4px; background: @selected; max-height: 8px; min-height: 8px; }
        QProgressBar::chunk { background: @primary; border-radius: 4px; }
        QListWidget { background: @raised; border: 1px solid @border; border-radius: 12px; padding: 6px; outline: 0; }
        QListWidget::item { min-height: 50px; padding: 0 12px; border-radius: 8px; }
        QListWidget::item:hover { background: @hover; }
        QListWidget::item:selected { background: @selected; color: @text; }
        QListWidget:focus { border: 2px solid @focus; }
        QScrollArea { border: none; background: transparent; }
        QScrollArea > QWidget > QWidget { background: transparent; }
        QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
        QScrollBar::handle:vertical { background: @control; border-radius: 5px; min-height: 32px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QToolTip { background: @raised; color: @text; border: 1px solid @control; padding: 8px; }
        QMenu { background: @raised; color: @text; border: 1px solid @border; padding: 6px; }
        QMenu::item { padding: 10px 20px; border-radius: 6px; }
        QMenu::item:selected { background: @selected; }
        QMenu::item:disabled { color: @disabled; }
        QMenu::separator { height: 1px; background: @border; margin: 4px 8px; }
        QDialog { background: @canvas; }
        QCheckBox { spacing: 10px; min-height: 40px; border: 2px solid transparent; border-radius: 8px; }
        QCheckBox:focus { border-color: @focus; }
    )");
    const QList<QPair<QString, QString>> substitutions = {
        {"primaryPressed", c.primaryPressed}, {"primaryHover", c.primaryHover}, {"onPrimary", c.onPrimary},
        {"placeholder", c.placeholder}, {"secondary", c.secondary}, {"disabled", c.disabled},
        {"successBg", c.successBg}, {"warningBg", c.warningBg}, {"infoBg", c.infoBg},
        {"success", c.success}, {"warning", c.warning}, {"info", c.info},
        {"selected", c.selected}, {"surface", c.surface}, {"canvas", c.canvas}, {"raised", c.raised},
        {"primary", c.primary}, {"control", c.control}, {"border", c.border}, {"hover", c.hover},
        {"focus", c.focus}, {"text", c.text}
    };
    for (const auto& pair : substitutions) qss.replace("@" + pair.first, pair.second);
    app.setStyleSheet(qss);
}

QIcon Theme::icon(const QString& name) const {
    QIcon result;
    for (int size : {20, 24, 40, 64}) {
        QPixmap pixmap(size * 2, size * 2);
        pixmap.setDevicePixelRatio(2);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(size / 24.0, size / 24.0);
        p.setPen(QPen(QColor(colors_.primary), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (name == "upload") {
            p.drawLine(12, 16, 12, 4); p.drawLine(12, 4, 7, 9); p.drawLine(12, 4, 17, 9);
            p.drawLine(4, 15, 4, 21); p.drawLine(4, 21, 20, 21); p.drawLine(20, 21, 20, 15);
        } else if (name == "map") {
            p.drawPolyline(QPolygonF{{3, 6}, {9, 3}, {15, 6}, {21, 3}, {21, 18}, {15, 21}, {9, 18}, {3, 21}, {3, 6}});
            p.drawLine(9, 3, 9, 18); p.drawLine(15, 6, 15, 21);
        } else if (name == "grid") {
            for (int x : {4, 14}) for (int y : {4, 14}) p.drawRoundedRect(QRectF(x, y, 6, 6), 1.5, 1.5);
        } else if (name == "settings") {
            p.drawEllipse(QPointF(12, 12), 8, 8); p.drawEllipse(QPointF(12, 12), 3, 3);
            for (int i = 0; i < 4; ++i) { p.drawLine(12, 1, 12, 4); p.translate(12,12); p.rotate(90); p.translate(-12,-12); }
        } else if (name == "folder") {
            p.drawPolyline(QPolygonF{{3, 7}, {3, 4}, {9, 4}, {12, 7}, {21, 7}, {21, 20}, {3, 20}, {3, 7}, {21, 7}});
        } else if (name == "clock") {
            p.drawEllipse(QPointF(12,12), 9, 9); p.drawLine(12,6,12,12); p.drawLine(12,12,16,14);
        } else if (name == "sun") {
            p.drawEllipse(QPointF(12,12),4,4);
            for (int i=0;i<8;++i) { p.drawLine(12,1,12,3); p.translate(12,12); p.rotate(45); p.translate(-12,-12); }
        } else {
            p.drawPolyline(QPolygonF{{8,5},{2,12},{8,19}}); p.drawPolyline(QPolygonF{{16,5},{22,12},{16,19}});
        }
        p.end();
        result.addPixmap(pixmap);
    }
    return result;
}
}
