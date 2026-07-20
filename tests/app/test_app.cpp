#include "app/appsettings.h"
#include "app/logger.h"
#include "diagnostics/diagnosticexporter.h"
#include "diagnostics/alertcoalescer.h"
#include "monitoring/healthmonitor.h"

#include <QSettings>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

using namespace MicArray;

class AppTest : public QObject
{
    Q_OBJECT

private slots:
    void invalidSettingsFallBackToSafeDefaults();
    void persistsNamedControlPresets();
    void rejectsInvalidControlPresets();
    void healthMonitorClassifiesFreshnessAndCalculatesRates();
    void loggerRotatesAndRetainsBoundedFiles();
    void alertCoalescerSuppressesRepeatedEventsWithinOneSecond();
    void exportsDiagnosticBundleWithSessionSettingsEventsAndLogs();
};

void AppTest::invalidSettingsFallBackToSafeDefaults()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    {
        QSettings raw(path, QSettings::IniFormat);
        raw.setValue(QStringLiteral("schemaVersion"), -1);
        raw.setValue(QStringLiteral("connection/host"), QStringLiteral("   "));
        raw.setValue(QStringLiteral("connection/port"), 70000);
        raw.setValue(QStringLiteral("audio/sampleRate"), 1);
        raw.setValue(QStringLiteral("audio/channelCount"), 999);
        raw.setValue(QStringLiteral("audio/outputChannel"), 999);
        raw.setValue(QStringLiteral("audio/averageWindowFrames"), 0);
        raw.setValue(QStringLiteral("video/format"), QStringLiteral("GIF"));
    }

    AppSettings settings(path);
    const ConnectionSettings connection = settings.connection();
    const AudioSettings audio = settings.audio();
    const VideoSettings video = settings.video();

    QCOMPARE(connection.host, QStringLiteral("192.168.2.10"));
    QCOMPARE(connection.port, quint16(9968));
    QCOMPARE(audio.sampleRate, 48000);
    QCOMPARE(audio.channelCount, 48);
    QCOMPARE(audio.outputChannel, 0);
    QCOMPARE(audio.averageWindowFrames, 100);
    QCOMPARE(video.format, QStringLiteral("JPG"));
}

void AppTest::persistsNamedControlPresets()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    AppSettings settings(path);
    ControlPreset preset;
    preset.name = QStringLiteral("演示场景");
    preset.values = {1, 2, 3, 4, 5, 6, 0, 0};

    QVERIFY(settings.saveControlPreset(preset));
    QCOMPARE(settings.controlPresets().size(), 1);
    QCOMPARE(settings.controlPresets().first(), preset);

    preset.values[0] = 42;
    QVERIFY(settings.saveControlPreset(preset));
    QCOMPARE(settings.controlPresets().size(), 1);
    QCOMPARE(settings.controlPresets().first().values[0], qint16(42));
    QVERIFY(settings.removeControlPreset(QStringLiteral("演示场景")));
    QVERIFY(settings.controlPresets().isEmpty());
}

void AppTest::rejectsInvalidControlPresets()
{
    QTemporaryDir directory;
    AppSettings settings(directory.filePath(QStringLiteral("settings.ini")));
    ControlPreset emptyName;
    emptyName.values = {1, 2, 3, 4, 5, 6, 0, 0};
    QVERIFY(!settings.saveControlPreset(emptyName));

    ControlPreset wrongSize;
    wrongSize.name = QStringLiteral("bad");
    wrongSize.values = {1, 2};
    QVERIFY(!settings.saveControlPreset(wrongSize));
}

void AppTest::healthMonitorClassifiesFreshnessAndCalculatesRates()
{
    HealthMonitor monitor;
    HealthInputs inputs;
    inputs.connected = true;
    inputs.lastReceiveAgeMs = 100;
    inputs.totalBytes = 1000;
    inputs.totalFrames = 10;

    HealthSnapshot first = monitor.update(inputs, 1000);
    QCOMPARE(first.level, HealthLevel::Healthy);
    QCOMPARE(first.receiveBytesPerSecond, 0.0);

    inputs.lastReceiveAgeMs = 2500;
    inputs.totalBytes = 3000;
    inputs.totalFrames = 30;
    inputs.parseErrors = 2;
    HealthSnapshot warning = monitor.update(inputs, 2000);
    QCOMPARE(warning.level, HealthLevel::Warning);
    QCOMPARE(warning.receiveBytesPerSecond, 2000.0);
    QCOMPARE(warning.framesPerSecond, 20.0);
    QCOMPARE(warning.parseErrorDelta, quint64(2));

    inputs.lastReceiveAgeMs = 5000;
    HealthSnapshot critical = monitor.update(inputs, 3000);
    QCOMPARE(critical.level, HealthLevel::Critical);
    QVERIFY(critical.reason.contains(QStringLiteral("超时")));

    inputs.connected = false;
    QCOMPARE(monitor.update(inputs, 4000).level, HealthLevel::Disconnected);
}

void AppTest::loggerRotatesAndRetainsBoundedFiles()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Logger logger(directory.path(), 120, 3);
    for (int index = 0; index < 20; ++index) {
        logger.info(QStringLiteral("test"), QString(40, QLatin1Char('x')));
    }

    const QStringList files = logger.logFiles();
    QVERIFY(files.size() > 1);
    QVERIFY(files.size() <= 3);
    QVERIFY(!logger.currentFilePath().isEmpty());
}

void AppTest::alertCoalescerSuppressesRepeatedEventsWithinOneSecond()
{
    AlertCoalescer coalescer;
    AlertDecision first = coalescer.record(QStringLiteral("parse-tail"), 0);
    QVERIFY(first.shouldEmit);
    QCOMPARE(first.suppressedCount, 0);
    QVERIFY(!coalescer.record(QStringLiteral("parse-tail"), 500).shouldEmit);
    AlertDecision summary = coalescer.record(QStringLiteral("parse-tail"), 1000);
    QVERIFY(summary.shouldEmit);
    QCOMPARE(summary.suppressedCount, 1);
    QVERIFY(coalescer.record(QStringLiteral("different"), 1001).shouldEmit);
}

void AppTest::exportsDiagnosticBundleWithSessionSettingsEventsAndLogs()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourceLog = directory.filePath(QStringLiteral("source.log"));
    QFile logFile(sourceLog);
    QVERIFY(logFile.open(QIODevice::WriteOnly));
    logFile.write("diagnostic log\n");
    logFile.close();

    DiagnosticBundle bundle;
    bundle.session.startedAt = QDateTime::fromString(QStringLiteral("2026-07-13T10:00:00"), Qt::ISODate);
    bundle.session.endedAt = QDateTime::fromString(QStringLiteral("2026-07-13T10:01:00"), Qt::ISODate);
    bundle.session.totalBytes = 1234;
    bundle.session.totalFrames = 56;
    bundle.session.reconnectCount = 2;
    bundle.settings.insert(QStringLiteral("host"), QStringLiteral("192.168.2.10"));
    bundle.events = {QStringLiteral("[INFO] connected"), QStringLiteral("[WARN] backpressure")};

    QString error;
    const QString exported = DiagnosticExporter::exportBundle(directory.path(), bundle, {sourceLog}, &error);
    QVERIFY2(!exported.isEmpty(), qPrintable(error));
    QVERIFY(QFileInfo::exists(QDir(exported).filePath(QStringLiteral("session.json"))));
    QVERIFY(QFileInfo::exists(QDir(exported).filePath(QStringLiteral("settings.json"))));
    QVERIFY(QFileInfo::exists(QDir(exported).filePath(QStringLiteral("events.txt"))));
    QVERIFY(QFileInfo::exists(QDir(exported).filePath(QStringLiteral("logs/source.log"))));

    QFile sessionFile(QDir(exported).filePath(QStringLiteral("session.json")));
    QVERIFY(sessionFile.open(QIODevice::ReadOnly));
    const QJsonObject session = QJsonDocument::fromJson(sessionFile.readAll()).object();
    QCOMPARE(session.value(QStringLiteral("totalBytes")).toInteger(), qint64(1234));
    QCOMPARE(session.value(QStringLiteral("reconnectCount")).toInteger(), qint64(2));
}

QTEST_MAIN(AppTest)
#include "test_app.moc"
