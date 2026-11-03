#include "app/MainWindow.h"
#include "ui/common/Theme.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QScreen>
#include <QTimer>
#include <cstdio>

int main(int argc, char* argv[]) {
    QApplication app(argc,argv);
    QApplication::setStyle("Fusion");
    QCoreApplication::setApplicationName("p1-navigation");
    QCoreApplication::setApplicationVersion("0.2.0");
    QCoreApplication::setOrganizationName("P1Linux");
    QCommandLineParser args;
    args.setApplicationDescription("P1 Linux navigation · offline road map, A* and FMM workflow");
    args.addHelpOption(); args.addVersionOption();
    args.addOption({"smoke","Show the window and exit after processing initial events."});
    args.addOption({"capture-dir","Capture application views for UI review, then exit.","directory"});
    args.process(app);
    p1::ui::Theme theme;
    theme.apply(app);
    p1::app::MainWindow window(theme);
    const auto available = window.screen()->availableGeometry().size();
    if (args.isSet("capture-dir")) {
        // Review captures intentionally exercise the documented reference sizes.
        window.resize(1440,900);
        window.show();
    } else if (available.width() < 1440 || available.height() < 900) {
        // Let the Wayland compositor account for panels and native decorations.
        window.showMaximized();
    } else {
        window.show();
    }
    if (args.isSet("capture-dir")) {
        const auto directory = args.value("capture-dir");
        if (!QDir().mkpath(directory)) return 2;
        // Separate event-loop turns let layouts and style changes settle before capture.
        auto capture = [&window,directory](const QString& name) {
            return window.grab().save(QDir(directory).filePath(name+".png"));
        };
        QTimer::singleShot(400,&window,[&window,&theme,&app,capture] {
            bool ok = capture("import-light");
            window.setCurrentPage("components");
            QTimer::singleShot(250,&window,[&window,&theme,&app,capture,ok]() mutable {
                ok = capture("components-light") && ok;
                theme.setDark(true); window.setCurrentPage("import");
                QTimer::singleShot(250,&window,[&window,&theme,&app,capture,ok]() mutable {
                    ok = capture("import-dark") && ok;
                    theme.setDark(false); window.resize(1024,720);
                    QTimer::singleShot(250,&window,[&app,capture,ok] {
                        app.exit(capture("import-compact") && ok ? 0 : 3);
                    });
                });
            });
        });
    } else if (args.isSet("smoke")) {
        QTimer::singleShot(300,&app,[&app]{std::puts("P1 shell startup: PASS"); app.quit();});
    }
    return app.exec();
}
