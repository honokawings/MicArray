#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QTimer>
#include <algorithm>

int main(int argc, char *argv[])
{
    Q_INIT_RESOURCE(reference);
    QApplication app(argc, argv);
    QApplication::setApplicationName("MicArray128");
    QApplication::setOrganizationName("MicArray");
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("128 螺旋阵列上位机；M128 UDP、SYS3 串口与本机 USB 摄像头"));
    parser.addHelpOption();
    parser.addOption({"receive", QStringLiteral("启动时开始接收")});
    parser.addOption({"serial-port", QStringLiteral("打开指定 SYS3 串口，只接收，不自动发送命令"), "COMn"});
    parser.addOption({"diagnose-seconds", QStringLiteral("接收指定秒数后导出报告并退出（真实联调）"), "seconds"});
    parser.addOption({"report", QStringLiteral("诊断 JSON 输出路径"), "path"});
    parser.addOption({"capture-array", QStringLiteral("配合定时诊断，录制 128 个独立原始音轨及 UDP 证据到新的子目录"), "directory"});
    parser.addOption({"smoke-seconds", QStringLiteral("仅启动界面，指定秒数后退出；不自动接收或打开摄像头"), "seconds"});
    parser.addOption({"screenshot", QStringLiteral("定时退出前保存界面快照"), "path"});
    parser.process(app);
    // Smoke runs may select virtual ports. Keep those preferences isolated
    // from the operator's production profile, just like the Qt UI tests.
    if (parser.isSet("smoke-seconds")) QCoreApplication::setApplicationName("MicArray128SmokeTests");
    if (parser.isSet("capture-array") && !parser.isSet("diagnose-seconds")) {
        qCritical() << "--capture-array requires --diagnose-seconds";
        return 2;
    }
    MicArray::Array128::MainWindow window;
    window.show();
    if (parser.isSet("serial-port")) window.openSerial(parser.value("serial-port"));
    // Starting a new receiver closes any previous recording session. Start the
    // bounded capture afterwards so it is not immediately stopped by start().
    if (parser.isSet("receive") || parser.isSet("diagnose-seconds")) window.startReceiving();
    if (parser.isSet("capture-array")) {
        MicArray::Array128::RecordingSettings recording;
        recording.directory = parser.value("capture-array");
        recording.tracks.clear();
        for (int ch = 0; ch < MicArray::Array128::Channels; ++ch) recording.tracks.append(ch);
        recording.metadata = {{"purpose", "bounded digital-chain capture; raw 128 channels, no acoustic processing"}};
        if (!window.pipeline().recorder().begin(recording)) return 2;
    }
    if (parser.isSet("diagnose-seconds") || parser.isSet("smoke-seconds")) {
        const QString duration = parser.isSet("diagnose-seconds") ? "diagnose-seconds" : "smoke-seconds";
        const int seconds = std::clamp(parser.value(duration).toInt(), 1, 3600);
        QTimer::singleShot(seconds * 1000, &window, [&] {
            if (parser.isSet("diagnose-seconds")) window.pipeline().stop();
            if (parser.isSet("diagnose-seconds") || parser.isSet("report"))
                window.saveDiagnosticReport(parser.value("report").isEmpty() ? "m128-diagnostics.json" : parser.value("report"));
            if (parser.isSet("screenshot")) window.grab().save(parser.value("screenshot"));
            app.quit();
        });
    }
    return app.exec();
}
