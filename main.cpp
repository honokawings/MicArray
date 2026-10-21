#include "ui/mainwindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("MicArray"));
    QCoreApplication::setApplicationName(QStringLiteral("MicArray"));
    QCoreApplication::setApplicationVersion(QStringLiteral("2026.10.21-pl-stream-fix-r2-test"));
    const bool smokeTest = a.arguments().contains(QStringLiteral("--smoke-test"));
    QTemporaryDir smokeSettings;
    if (smokeTest)
    {
        QCoreApplication::setOrganizationName(QStringLiteral("MicArrayReleaseSmoke"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, smokeSettings.path());
    }
    else
    {
        // Trial builds read the existing configuration once, then write only
        // their own portable INI file. The running release registry is untouched.
        QSettings previous;
        QCoreApplication::setOrganizationName(QStringLiteral("MicArrayPLFullV3Trial"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("settings")));
        QSettings portable;
        if (!portable.value(QStringLiteral("plDenoiseTrial/initialized"), false).toBool())
        {
            for (const QString &key : previous.allKeys())
            {
                portable.setValue(key, previous.value(key));
            }
            portable.setValue(QStringLiteral("plDenoiseTrial/initialized"), true);
            portable.sync();
        }
    }
    MicArray::MainWindow w;
    if (smokeTest)
    {
        QTimer::singleShot(300, &a, &QCoreApplication::quit);
    }
    else
    {
        w.setWindowTitle(w.windowTitle() + QStringLiteral(" — PL降噪连续流修订 R2（测试版）"));
        w.show();
    }
    return QApplication::exec();
}
