#pragma once
#include <QMainWindow>
#include <QMap>

class QLabel;
class QPushButton;
class QStackedWidget;
class QHBoxLayout;
class QVBoxLayout;
class QFrame;
namespace p1::ui { class Theme; }
namespace p1::app {
class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(ui::Theme& theme, QWidget* parent = nullptr, const QString& importCacheDirectory = {});
    // GUI thread only. On success, transfers ownership to this window; on failure,
    // ownership stays unchanged. One widget may belong to only one route.
    // Replacement keeps the current route; re-registering the same widget updates metadata.
    // Intended for independently built module QWidget factories, not database access.
    bool registerPage(const QString& id, const QString& title,
                      const QString& description, QWidget* page);
    bool setCurrentPage(const QString& id);
    QString currentPageId() const { return currentId_; }
    bool isCompact() const { return compact_; }
protected:
    void resizeEvent(QResizeEvent* event) override;
private:
    struct Page { QString title, description; QWidget* widget; };
    QWidget* makeGallery();
    QWidget* makeSettings();
    QPushButton* addNavigation(const QString& id, const QString& text, const QString& icon, bool enabled);
    void refreshNavigation();
    void showHelp();
    ui::Theme& theme_;
    QFrame* sidebar_;
    QVBoxLayout* navigation_;
    QHBoxLayout* shell_;
    QStackedWidget* pages_;
    QLabel* title_;
    QLabel* description_;
    QLabel* logo_;
    QLabel* toolsLabel_;
    QLabel* sidebarFoot_;
    QPushButton* themeButton_;
    QMap<QString, Page> registry_;
    QMap<QString, QPushButton*> buttons_;
    QString currentId_;
    bool compact_ = false;
};
}
