// Process/lifecycle test helper ONLY. This is not FMM and cannot validate HMM.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    auto arg = [&](const QString& key) {
        const int i = args.indexOf(key);
        return i >= 0 && i + 1 < args.size() ? args[i + 1] : QString{};
    };
    const auto network = arg(QStringLiteral("--network"));
    QFile marker(network + QStringLiteral(".started"));
    if (!marker.open(QIODevice::WriteOnly)) return 2;
    marker.close();
    QElapsedTimer timer;
    timer.start();
    while (!QFile::exists(network + QStringLiteral(".release"))) {
        if (timer.elapsed() > 10000) return 3;
        QThread::msleep(5);
    }
    QFile mode(arg(QStringLiteral("--ubodt")));
    if (!mode.open(QIODevice::ReadOnly)) return 7;
    const auto action = mode.readAll();
    if (action == "exit-without-output") return 0;
    if (action == "fail") return 9;
    QFile gps(arg(QStringLiteral("--gps")));
    if (!gps.open(QIODevice::ReadOnly)) return 4;
    const auto input = gps.readAll().split('\n');
    if (input.size() < 2) return 4;
    const auto wkt = input[1].mid(2);
    const int count = wkt.count(',') + 1;
    QByteArray ids, errors;
    for (int i = 0; i < count; ++i) {
        if (i) { ids += ','; errors += ','; }
        ids += "-7"; errors += '0';
    }
    QFile output(arg(QStringLiteral("--output")));
    if (!output.open(QIODevice::WriteOnly)) return 5;
    const QByteArray data = "id;opath;error;pgeom;cpath;mgeom\n1;" + ids + ';' + errors + ';' + wkt + ";-7;" + wkt + '\n';
    return output.write(data) == data.size() ? 0 : 6;
}
