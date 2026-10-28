#include "app/MainWindow.h"
#include "app/ImportLanding.h"
#include "ui/common/Components.h"
#include "ui/common/Theme.h"
#include "ui/map/MapDemoWidget.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QScreen>
#include <QSpinBox>
#include <QStackedWidget>

namespace p1::app {
using namespace ui;
MainWindow::MainWindow(Theme& theme, QWidget* parent) : QMainWindow(parent), theme_(theme) {
    setWindowTitle("P1 导航 · Linux");
    setObjectName("p1MainWindow");
    setMinimumSize(1024,720);
    const auto* screen = QGuiApplication::primaryScreen();
    resize(screen ? QSize(1440,900).boundedTo(screen->availableGeometry().size())
                  : QSize(1440,900));
    auto* canvas = new QWidget;
    canvas->setObjectName("canvas");
    setCentralWidget(canvas);
    shell_ = new QHBoxLayout(canvas);
    shell_->setContentsMargins(24,24,24,24); shell_->setSpacing(24);
    sidebar_ = new QFrame;
    sidebar_->setObjectName("sidebar"); sidebar_->setFixedWidth(192);
    auto* sidebarLayout = new QVBoxLayout(sidebar_);
    sidebarLayout->setContentsMargins(12,24,12,20); sidebarLayout->setSpacing(12);
    logo_ = label("P1 导航", "sectionTitle");
    logo_->setAlignment(Qt::AlignCenter);
    sidebarLayout->addWidget(logo_);
    navigation_ = new QVBoxLayout;
    navigation_->setSpacing(8);
    sidebarLayout->addLayout(navigation_);
    addNavigation("import","数据导入","upload",true);
    addNavigation("map","地图导航","map",false);
    addNavigation("algorithms","算法实验","code",false);
    addNavigation("offline","离线数据","folder",false);
    addNavigation("history","收藏历史","clock",false);
    sidebarLayout->addStretch();
    sidebarLayout->addWidget(label("第一次作业", "caption"));
    addNavigation("components","组件预览","grid",true);
    // Move the remaining navigation buttons to the bottom without making another registry.
    sidebarLayout->addWidget(buttons_["components"]);
    addNavigation("settings","设置与帮助","settings",true);
    sidebarLayout->addWidget(buttons_["settings"]);
    sidebarFoot_ = label("LINUX DESKTOP\n本地 · 离线优先", "caption");
    sidebarFoot_->setAlignment(Qt::AlignCenter);
    sidebarLayout->addWidget(sidebarFoot_);
    shell_->addWidget(sidebar_);

    auto* workspace = new QWidget;
    auto* work = new QVBoxLayout(workspace);
    work->setContentsMargins(0,0,0,0); work->setSpacing(24);
    auto* header = new QHBoxLayout;
    auto* headings = new QVBoxLayout;
    headings->setSpacing(8);
    title_ = label({},"pageTitle"); title_->setObjectName("pageTitle");
    description_ = label({},"secondary");
    headings->addWidget(title_); headings->addWidget(description_);
    header->addLayout(headings,1);
    auto* phase = badge("第 1 次作业");
    header->addWidget(phase,0,Qt::AlignTop);
    themeButton_ = button("深色主题");
    themeButton_->setObjectName("themeButton");
    themeButton_->setCheckable(true);
    themeButton_->setAccessibleName("切换浅色或深色主题");
    header->addWidget(themeButton_,0,Qt::AlignTop);
    connect(themeButton_, &QPushButton::clicked, &theme_, &Theme::setDark);
    work->addLayout(header);
    pages_ = new QStackedWidget;
    pages_->setObjectName("pageStack");
    work->addWidget(pages_,1);
    auto* status = new QHBoxLayout;
    status->addWidget(label("●  本地会话", "caption"));
    status->addStretch();
    status->addWidget(label("解析服务待接入  /  定位、语音尚未启用", "caption"));
    work->addLayout(status);
    shell_->addWidget(workspace,1);

    registerPage("import","数据导入","添加本地路网、兴趣点或轨迹数据。",new ImportLanding(theme));
    registerPage("map","地图导航","固定场景地图与导航路线演示。",new MapDemoWidget());
    registerPage("components","组件预览","检查公共控件与交互状态。此页为独立演示，不导入业务数据。",makeGallery());
    registerPage("settings","设置与帮助","让工作台适合你。偏好仅在本次会话中保留。",makeSettings());
    connect(&theme_, &Theme::changed, this, &MainWindow::refreshNavigation);
    auto* help = new QShortcut(QKeySequence::HelpContents,this);
    connect(help,&QShortcut::activated,this,&MainWindow::showHelp);
    auto* importShortcut = new QShortcut(QKeySequence("Ctrl+1"),this);
    connect(importShortcut,&QShortcut::activated,this,[this]{setCurrentPage("import");});
    auto* settingsShortcut = new QShortcut(QKeySequence("Ctrl+,"),this);
    connect(settingsShortcut,&QShortcut::activated,this,[this]{setCurrentPage("settings");});
    setCurrentPage("import");
    refreshNavigation();
}

QPushButton* MainWindow::addNavigation(const QString& id, const QString& text, const QString& icon, bool enabled) {
    auto* result = button(text);
    result->setObjectName("nav_" + id);
    result->setProperty("nav",true);
    result->setProperty("fullText",text);
    result->setProperty("iconName",icon);
    result->setAccessibleName(text);
    result->setIconSize(QSize(20,20));
    result->setCheckable(true); result->setEnabled(enabled);
    result->setToolTip(enabled ? text : text + "：后续作业接入");
    connect(result,&QPushButton::clicked,this,[this,id]{setCurrentPage(id);});
    buttons_.insert(id,result); navigation_->addWidget(result);
    return result;
}

bool MainWindow::registerPage(const QString& id, const QString& title, const QString& description, QWidget* page) {
    if (!page || !buttons_.contains(id)) return false;
    if (registry_.contains(id)) {
        auto* previous = registry_[id].widget;
        if (previous == page) return true;
        pages_->removeWidget(previous);
        previous->deleteLater();
    }
    pages_->addWidget(page);
    registry_.insert(id,Page{title,description,page});
    buttons_[id]->setEnabled(true);
    buttons_[id]->setToolTip(title);
    if (currentId_ == id) setCurrentPage(id);
    return true;
}

bool MainWindow::setCurrentPage(const QString& id) {
    const auto found = registry_.constFind(id);
    if (found == registry_.cend()) return false;
    currentId_ = id;
    pages_->setCurrentWidget(found->widget);
    title_->setText(found->title); description_->setText(found->description);
    for (auto it=buttons_.begin();it!=buttons_.end();++it) it.value()->setChecked(it.key()==id);
    return true;
}

void MainWindow::refreshNavigation() {
    for (auto* b : buttons_) {
        b->setIcon(theme_.icon(b->property("iconName").toString()));
        b->setText(compact_ ? QString() : b->property("fullText").toString());
    }
    logo_->setText(compact_ ? "P1" : "P1 导航");
    sidebarFoot_->setVisible(!compact_);
    themeButton_->setChecked(theme_.isDark());
    themeButton_->setText(theme_.isDark() ? "浅色主题" : "深色主题");
    themeButton_->setIcon(theme_.icon("sun"));
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    compact_ = width() < 1280;
    sidebar_->setFixedWidth(compact_ ? 72 : 192);
    const int margin = compact_ ? 16 : 24;
    shell_->setContentsMargins(margin,margin,margin,margin);
    shell_->setSpacing(margin);
    refreshNavigation();
}

QWidget* MainWindow::makeGallery() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(6,0,6,12); layout->setSpacing(24);
    auto* notice = badge("i  组件演示 · 所有数值由你手动调整");
    layout->addWidget(notice);
    auto* card = new Card(theme_);
    card->body()->addWidget(label("表单与进度", "sectionTitle"));
    auto* form = new QFormLayout;
    form->setSpacing(16);
    auto* name = new QLineEdit;
    name->setObjectName("demoName"); name->setPlaceholderText("例如：校园周边数据");
    name->setAccessibleName("演示数据集名称");
    form->addRow("数据集名称",name);
    auto* progressValue = new QSpinBox;
    progressValue->setObjectName("demoProgressValue");
    progressValue->setButtonSymbols(QAbstractSpinBox::NoButtons);
    progressValue->setRange(0,100); progressValue->setSuffix(" %"); progressValue->setValue(40);
    progressValue->setAccessibleName("手动设置演示进度");
    form->addRow("演示进度",progressValue);
    card->body()->addLayout(form);
    auto* progressText = label("演示进度：40%", "secondary");
    auto* progress = new QProgressBar;
    progress->setObjectName("demoProgress"); progress->setValue(40); progress->setTextVisible(false);
    progress->setAccessibleName("组件演示进度条");
    card->body()->addWidget(progressText); card->body()->addWidget(progress);
    auto* unknown = new QCheckBox("总量未知：展示不定进度");
    unknown->setObjectName("demoIndeterminate");
    card->body()->addWidget(unknown);
    connect(progressValue,&QSpinBox::valueChanged,card,[progress,progressText](int value) {
        progress->setValue(value); progressText->setText(QString("演示进度：%1%").arg(value));
    });
    connect(unknown,&QCheckBox::toggled,card,[progress,progressValue,progressText](bool checked) {
        progressValue->setEnabled(!checked);
        progress->setRange(0,checked ? 0 : 100);
        if (!checked) progress->setValue(progressValue->value());
        progressText->setText(checked ? "演示：总量未知，等待任务返回进度" : QString("演示进度：%1%").arg(progressValue->value()));
    });
    auto* actions = new QHBoxLayout;
    auto* dialogButton = button("打开演示对话框",true);
    dialogButton->setObjectName("demoDialogButton");
    connect(dialogButton,&QPushButton::clicked,this,[this] {
        auto* dialog = new QDialog(this);
        dialog->setObjectName("demoDialog");
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle("组件演示"); dialog->setMinimumWidth(480);
        auto* body = new QVBoxLayout(dialog);
        body->setContentsMargins(24,24,24,24); body->setSpacing(24);
        body->addWidget(label("准备好开始了吗？","sectionTitle"));
        body->addWidget(label("此对话框演示确认、取消和键盘焦点。确认后只关闭窗口，不会修改数据。","secondary"));
        auto* buttons = new QHBoxLayout;
        buttons->setSpacing(16);
        auto* cancel = button("取消");
        auto* confirm = button("确认",true);
        buttons->addStretch();
        buttons->addWidget(cancel);
        buttons->addWidget(confirm);
        connect(cancel,&QPushButton::clicked,dialog,&QDialog::reject);
        connect(confirm,&QPushButton::clicked,dialog,&QDialog::accept);
        body->addLayout(buttons);
        dialog->setWindowModality(Qt::WindowModal);
        dialog->open(); cancel->setFocus();
    });
    actions->addWidget(dialogButton);
    auto* disabled = button("不可用操作"); disabled->setEnabled(false);
    disabled->setToolTip("此按钮用于展示禁用状态");
    actions->addWidget(disabled); actions->addStretch();
    card->body()->addLayout(actions);
    layout->addWidget(card);
    auto* listCard = new Card(theme_);
    listCard->body()->addWidget(label("列表与状态", "sectionTitle"));
    auto* stateRow = new QHBoxLayout;
    stateRow->addWidget(badge("✓  可用","success"));
    stateRow->addWidget(badge("!  等待接入","warning"));
    stateRow->addStretch(); listCard->body()->addLayout(stateRow);
    auto* list = new QListWidget;
    list->setObjectName("demoList"); list->setAccessibleName("公共组件演示列表");
    list->addItems({"卡片  /  统一容器、圆角与阴影", "表单  /  标签、输入与键盘焦点", "进度  /  确定进度与不定进度"});
    list->setCurrentRow(0); list->setMinimumHeight(180);
    listCard->body()->addWidget(list); layout->addWidget(listCard);
    layout->addStretch();
    return scrollable(content);
}

QWidget* MainWindow::makeSettings() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(6,0,6,12); layout->setSpacing(24);
    auto* appearance = new Card(theme_);
    appearance->body()->addWidget(label("外观", "sectionTitle"));
    appearance->body()->addWidget(label("使用淡紫色工作台，或切换至适合低光环境的深色主题。", "secondary"));
    auto* form = new QFormLayout;
    auto* mode = new QComboBox;
    mode->setObjectName("themeCombo"); mode->setAccessibleName("界面主题");
    mode->addItems({"浅色","深色"});
    form->addRow("界面主题",mode);
    appearance->body()->addLayout(form);
    appearance->body()->addWidget(badge("i  仅本次会话 · 本地偏好存储待接入"));
    connect(mode,&QComboBox::currentIndexChanged,this,[this](int index){ theme_.setDark(index==1); });
    connect(&theme_,&Theme::changed,mode,[this,mode]{
        const QSignalBlocker block(mode); mode->setCurrentIndex(theme_.isDark()?1:0);
    });
    layout->addWidget(appearance);
    auto* help = new Card(theme_);
    help->body()->addWidget(label("从这里开始", "sectionTitle"));
    help->body()->addWidget(label("Ctrl+1  返回数据导入\nCtrl+,  打开设置\nTab / Shift+Tab  移动焦点\nF1  查看帮助\nEsc  关闭对话框", "secondary"));
    auto* helpButton = button("关于当前版本");
    connect(helpButton,&QPushButton::clicked,this,&MainWindow::showHelp);
    help->body()->addWidget(helpButton);
    layout->addWidget(help); layout->addStretch();
    return scrollable(content);
}

void MainWindow::showHelp() {
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("关于 P1 导航"); dialog->setMinimumWidth(480);
    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(24,24,24,24); layout->setSpacing(24);
    layout->addWidget(label("P1 导航 · 0.1.0", "sectionTitle"));
    layout->addWidget(label("第一次作业应用框架：页面导航、明暗主题和公共组件。\n\n文件解析与保存由 2 号模块接入；算法、地图、定位与语音将在后续作业实现。", "secondary"));
    auto* close = button("知道了",true);
    connect(close,&QPushButton::clicked,dialog,&QDialog::accept);
    layout->addWidget(close); dialog->open(); close->setFocus();
}
}
