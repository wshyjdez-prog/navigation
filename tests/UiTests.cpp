#include "app/MainWindow.h"
#include "app/ImportPage.h"
#include "ui/common/Theme.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QListWidget>
#include <QtTest>

class UiTests : public QObject {
    Q_OBJECT
private slots:
    void init() {
        theme_ = new p1::ui::Theme(this);
        theme_->apply(*qApp);
        cache_ = new QTemporaryDir;
        window_ = new p1::app::MainWindow(*theme_,nullptr,cache_->path());
        window_->show();
        QVERIFY(QTest::qWaitForWindowExposed(window_));
    }
    void cleanup() { delete window_; delete theme_; delete cache_; }

    void initialClientFitsAvailableScreen() {
        const auto available = window_->screen()->availableGeometry().size();
        // On undersized test displays the documented minimum remains authoritative.
        QVERIFY(window_->width() <= qMax(1024,available.width()));
        QVERIFY(window_->height() <= qMax(720,available.height()));
    }

    void themeAndNavigationPreserveForm() {
        QVERIFY(window_->setCurrentPage("components"));
        auto* input = window_->findChild<QLineEdit*>("demoName");
        input->setText("校园数据");
        auto* progress = window_->findChild<QSpinBox*>("demoProgressValue");
        progress->setValue(63);
        QTest::mouseClick(window_->findChild<QPushButton*>("themeButton"),Qt::LeftButton);
        QVERIFY(theme_->isDark());
        QCOMPARE(window_->findChild<QComboBox*>("themeCombo")->currentIndex(),1);
        QCOMPARE(window_->currentPageId(),QString("components"));
        QTest::mouseClick(window_->findChild<QPushButton*>("nav_settings"),Qt::LeftButton);
        QCOMPARE(window_->currentPageId(),QString("settings"));
        window_->findChild<QComboBox*>("themeCombo")->setCurrentIndex(0);
        QVERIFY(!theme_->isDark());
        QVERIFY(window_->setCurrentPage("components"));
        QCOMPARE(input->text(),QString("校园数据"));
        QCOMPARE(progress->value(),63);
        QCOMPARE(window_->findChild<QProgressBar*>("demoProgress")->value(),63);
        auto* unknown = window_->findChild<QCheckBox*>("demoIndeterminate");
        unknown->setChecked(true);
        QCOMPARE(window_->findChild<QProgressBar*>("demoProgress")->maximum(),0);
        unknown->setChecked(false);
        QCOMPARE(window_->findChild<QProgressBar*>("demoProgress")->value(),63);
    }

    void disabledRoutesDoNotChangePage() {
        QVERIFY(window_->setCurrentPage("map"));
        QVERIFY(window_->setCurrentPage("algorithms"));
        QVERIFY(!window_->setCurrentPage("unknown"));
        QCOMPARE(window_->currentPageId(),QString("algorithms"));
        QVERIFY(window_->findChild<QPushButton*>("nav_map")->isEnabled());
        QVERIFY(window_->findChild<QPushButton*>("nav_algorithms")->isEnabled());
        QVERIFY(!window_->findChild<QPushButton*>("startImportButton")->isEnabled());
    }

    void mapPagePublishesAsynchronousFrame() {
        QVERIFY(window_->setCurrentPage("map"));
        auto* canvas = window_->findChild<QWidget*>("mapRenderCanvas");
        auto* status = window_->findChild<QLabel*>("mapRenderStats");
        QVERIFY(canvas);
        QVERIFY(status);
        QTRY_VERIFY_WITH_TIMEOUT(canvas->property("hasFrame").toBool(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(status->text().contains("绘制"), 5000);
        const QString captures = qEnvironmentVariable("P1_UI_CAPTURE_DIR");
        if (!captures.isEmpty()) {
            QVERIFY(QDir().mkpath(captures));
            QVERIFY(window_->grab().save(QDir(captures).filePath("map-light.png")));
            theme_->setDark(true);
            QVERIFY(window_->grab().save(QDir(captures).filePath("map-dark.png")));
        }
    }

    void selectedFileSurvivesThemeAndInvalidSelection() {
        QTemporaryFile file(QDir::tempPath()+"/p1-selected-XXXXXX.osm");
        QVERIFY(file.open()); file.write("unparsed fixture"); file.flush();
        auto* landing = window_->findChild<p1::app::ImportPage*>();
        QVERIFY(landing);
        landing->selectFile(file.fileName());
        QCOMPARE(landing->selectedFile(),file.fileName());
        QVERIFY(window_->findChild<QPushButton*>("startImportButton")->isEnabled());
        theme_->setDark(true);
        window_->setCurrentPage("settings"); window_->setCurrentPage("import");
        QCOMPARE(landing->selectedFile(),file.fileName());
        landing->selectFile(file.fileName()+".missing");
        QCOMPARE(landing->selectedFile(),file.fileName());
        QVERIFY(!window_->findChild<QPushButton*>("startImportButton")->isEnabled());
    }

    void pageReplacementTransfersOwnership() {
        auto* stack = window_->findChild<QStackedWidget*>("pageStack");
        QPointer<QWidget> oldPage = stack->currentWidget();
        auto* replacement = new QWidget;
        QVERIFY(window_->registerPage("import","已接入页面","测试模块替换",replacement));
        QCOMPARE(stack->currentWidget(),replacement);
        QCOMPARE(stack->count(),5);
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        QVERIFY(oldPage.isNull());
        QVERIFY(window_->registerPage("map","地图模块","测试注册",new QWidget));
        QVERIFY(window_->findChild<QPushButton*>("nav_map")->isEnabled());
        QVERIFY(window_->setCurrentPage("map"));
        QWidget rejected;
        QVERIFY(!window_->registerPage("unknown","","",&rejected));
        QVERIFY(!window_->registerPage("import","","",nullptr));
    }

    void modulePagesRetainStateAcrossNavigationAndReplacement() {
        auto* algorithms = new QLineEdit;
        algorithms->setText("待提交的起终点输入");
        QVERIFY(window_->registerPage("algorithms","算法实验","测试工厂页面",algorithms));
        QCOMPARE(window_->currentPageId(),QString("import"));
        QVERIFY(window_->setCurrentPage("algorithms"));
        QPointer<QWidget> oldMap = new QWidget;
        QVERIFY(window_->registerPage("map","地图导航","测试工厂页面",oldMap));
        QVERIFY(window_->setCurrentPage("map"));
        theme_->setDark(true);
        QVERIFY(window_->setCurrentPage("algorithms"));
        QCOMPARE(algorithms->text(),QString("待提交的起终点输入"));
        auto* newMap = new QWidget;
        QVERIFY(window_->registerPage("map","新地图","替换后台页面",newMap));
        QCOMPARE(window_->currentPageId(),QString("algorithms"));
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        QVERIFY(oldMap.isNull());
        QVERIFY(window_->setCurrentPage("map"));
        QCOMPARE(window_->findChild<QStackedWidget*>("pageStack")->currentWidget(),newMap);
    }

    void registrationRejectsSharedPagesAndOwnershipCycles() {
        auto* stack = window_->findChild<QStackedWidget*>("pageStack");
        auto* importPage = stack->currentWidget();
        const int count = stack->count();
        QVERIFY(!window_->registerPage("map","错误复用","",importPage));
        QVERIFY(!window_->registerPage("import","错误容器","",stack));
        QVERIFY(!window_->registerPage("import","错误窗口","",window_));
        auto* child = new QWidget(importPage);
        QVERIFY(!window_->registerPage("map","错误子页","",child));
        QVERIFY(!window_->registerPage("import","错误子页","",child));
        QCOMPARE(child->parentWidget(),importPage);
        QCOMPARE(stack->count(),count);
        QCOMPARE(stack->currentWidget(),importPage);
        QVERIFY(window_->findChild<QPushButton*>("nav_map")->isEnabled());
    }

    void samePageRegistrationRefreshesMetadataWithoutDeletingPage() {
        auto* stack = window_->findChild<QStackedWidget*>("pageStack");
        QPointer<QWidget> page = stack->currentWidget();
        const int count = stack->count();
        QVERIFY(window_->registerPage("import","更新后的标题","更新后的说明",page));
        QCOMPARE(window_->findChild<QLabel*>("pageTitle")->text(),QString("更新后的标题"));
        QCOMPARE(window_->findChild<QPushButton*>("nav_import")->toolTip(),QString("更新后的标题"));
        QCOMPARE(stack->count(),count);
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        QVERIFY(page);
        QCOMPARE(stack->currentWidget(),page.data());
    }

    void compactLayoutAndKeyboardDialog() {
        window_->resize(1024,720);
        QTRY_VERIFY(window_->isCompact());
        QCOMPARE(window_->size(),QSize(1024,720));
        auto* choose = window_->findChild<QPushButton*>("chooseFileButton");
        QVERIFY(choose->isVisible());
        const QRect viewport(QPoint(0,0),window_->size());
        QVERIFY(viewport.contains(QRect(choose->mapTo(window_,QPoint(0,0)),choose->size())));
        QVERIFY(window_->findChild<QPushButton*>("nav_import")->text().isEmpty());
        QCOMPARE(window_->findChild<QPushButton*>("nav_import")->accessibleName(),QString("数据导入"));
        window_->setCurrentPage("components");
        QTest::mouseClick(window_->findChild<QPushButton*>("demoDialogButton"),Qt::LeftButton);
        auto* dialog = window_->findChild<QDialog*>("demoDialog");
        QVERIFY(dialog); QVERIFY(dialog->isVisible());
        QTest::keyClick(dialog,Qt::Key_Escape);
        QTRY_VERIFY(window_->findChild<QDialog*>("demoDialog")==nullptr);
        window_->resize(1440,900);
        QTRY_VERIFY(!window_->isCompact());
        QCOMPARE(window_->findChild<QPushButton*>("nav_import")->text(),QString("数据导入"));
    }
    void importPersistsAndCatalogReloads() {
        QTemporaryFile file(QDir::tempPath()+"/p1-ui-XXXXXX.osm");
        QVERIFY(file.open());
        file.write("<osm version=\"0.6\"><node id=\"1\" lat=\"30.5\" lon=\"114.3\"/></osm>"); file.flush();
        auto* page=window_->findChild<p1::app::ImportPage*>(); QVERIFY(page);
        QSignalSpy done(page,&p1::app::ImportPage::importFinished);
        page->selectFile(file.fileName()); page->startImport();
        QVERIFY(page->isImporting());
        theme_->setDark(true); QVERIFY(window_->setCurrentPage("settings"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,10000);
        const auto result=qvariant_cast<p1::app::ImportResult>(done.at(0).at(0));
        QVERIFY2(result.ok,qPrintable(result.error)); QVERIFY(QFile::exists(result.cachePath));
        QVERIFY(!page->isImporting());
        p1::app::ImportPage reopened(*theme_,nullptr,cache_->path());
        auto* list=reopened.findChild<QListWidget*>("importedDatasets");
        QTRY_VERIFY_WITH_TIMEOUT(list->count()==1 && list->item(0)->text().contains(QFileInfo(file.fileName()).fileName()),10000);
    }
private:
    QTemporaryDir* cache_ = nullptr;
    p1::ui::Theme* theme_ = nullptr;
    p1::app::MainWindow* window_ = nullptr;
};
QTEST_MAIN(UiTests)
#include "UiTests.moc"
