#include "app/appsettings.h"
#include "app/logger.h"
#include "diagnostics/diagnosticexporter.h"
#include "diagnostics/alertcoalescer.h"
#include "media/channelgainprocessor.h"
#include "monitoring/healthmonitor.h"

#include <QSettings>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include <functional>
#include <limits>

using namespace MicArray;

class AppTest : public QObject
{
    Q_OBJECT

private slots:
    void invalidSettingsFallBackToSafeDefaults();
    void persistsNamedControlPresets();
    void migratesLegacyEightChannelControlPresets();
    void rejectsInvalidControlPresets();
    void gainConfigurationDefaultsToValidValues();
    void persistsGainConfigurationAcrossSettingsInstances();
    void persistsScientificNotationGainConfigurationAfterColdStart();
    void rejectsInvalidGainConfigurationWithoutReplacingStoredValue();
    void rejectsCorruptedGainConfigurationValueTypes();
    void loadsCanonicalIniStringGainConfigurationAfterColdStart();
    void rejectsNonCanonicalIniStringGainConfigurationAfterColdStart();
    void restoresGainConfigurationAfterSyncFailure();
    void persistsAudioAnalysisSettings();
    void invalidAudioAnalysisFieldsFallBackIndependently();
    void persistsLiveLocalizationSettings();
    void invalidLiveLocalizationFieldsFallBackIndependently();
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
    preset.values = {1, 2, 3, 4, 5, 6};

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

void AppTest::migratesLegacyEightChannelControlPresets()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    {
        QSettings raw(path, QSettings::IniFormat);
        raw.beginWriteArray(QStringLiteral("controlPresets"));
        raw.setArrayIndex(0);
        raw.setValue(QStringLiteral("name"), QStringLiteral("legacy"));
        raw.setValue(QStringLiteral("values"), QVariantList {1, 2, 3, 4, 5, 6, 7, 8});
        raw.setArrayIndex(1);
        raw.setValue(QStringLiteral("name"), QStringLiteral("invalid-five"));
        raw.setValue(QStringLiteral("values"), QVariantList {1, 2, 3, 4, 5});
        raw.setArrayIndex(2);
        raw.setValue(QStringLiteral("name"), QStringLiteral("invalid-seven"));
        raw.setValue(QStringLiteral("values"), QVariantList {1, 2, 3, 4, 5, 6, 7});
        raw.endArray();
    }

    AppSettings settings(path);
    const QList<ControlPreset> presets = settings.controlPresets();
    QCOMPARE(presets.size(), 1);
    QCOMPARE(presets.first().name, QStringLiteral("legacy"));
    QCOMPARE(presets.first().values, QVector<qint16>({1, 2, 3, 4, 5, 6}));
}

void AppTest::rejectsInvalidControlPresets()
{
    QTemporaryDir directory;
    AppSettings settings(directory.filePath(QStringLiteral("settings.ini")));
    ControlPreset emptyName;
    emptyName.values = {1, 2, 3, 4, 5, 6};
    QVERIFY(!settings.saveControlPreset(emptyName));

    ControlPreset wrongSize;
    wrongSize.name = QStringLiteral("bad");
    wrongSize.values = {1, 2};
    QVERIFY(!settings.saveControlPreset(wrongSize));

    wrongSize.values = {1, 2, 3, 4, 5, 6, 7, 8};
    QVERIFY(!settings.saveControlPreset(wrongSize));
}

void AppTest::gainConfigurationDefaultsToValidValues()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AppSettings settings(directory.filePath(QStringLiteral("settings.ini")));
    ChannelGainProcessor processor;
    QString error;

    QVERIFY2(processor.setConfiguration(settings.gainConfiguration(), &error), qPrintable(error));
}

void AppTest::persistsGainConfigurationAcrossSettingsInstances()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    GainConfiguration configuration;
    for (int channel = 0; channel < GainConfiguration::ChannelCount; ++channel) {
        configuration.shifts[channel] = channel % 17 - 8;
        configuration.automatic[channel] = channel % 2 == 1;
    }
    configuration.upperPeakRatio = 0.77;
    configuration.lowerPeakRatio = 0.31;
    configuration.releaseHoldMs = 740;
    configuration.releaseStepMs = 190;

    {
        AppSettings settings(path);
        QString error;
        QVERIFY2(settings.saveGainConfiguration(configuration, &error), qPrintable(error));
    }

    AppSettings reloaded(path);
    QCOMPARE(reloaded.gainConfiguration(), configuration);
}

void AppTest::persistsScientificNotationGainConfigurationAfterColdStart()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    GainConfiguration configuration;
    configuration.upperPeakRatio = 1e-6;
    configuration.lowerPeakRatio = 0.0;

    {
        AppSettings settings(path);
        QString error;
        QVERIFY2(settings.saveGainConfiguration(configuration, &error), qPrintable(error));
    }

    AppSettings reloaded(path);
    QCOMPARE(reloaded.gainConfiguration(), configuration);
}

void AppTest::rejectsInvalidGainConfigurationWithoutReplacingStoredValue()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AppSettings settings(directory.filePath(QStringLiteral("settings.ini")));
    GainConfiguration baseline;
    baseline.shifts[9] = -5;
    baseline.automatic[9] = true;
    QVERIFY(settings.saveGainConfiguration(baseline));

    QList<QPair<GainConfiguration, QString>> invalidConfigurations;
    GainConfiguration invalid = baseline;
    invalid.shifts[0] = 9;
    invalidConfigurations.append({invalid, QStringLiteral("shift")});
    invalid = baseline;
    invalid.upperPeakRatio = 0.0;
    invalidConfigurations.append({invalid, QStringLiteral("upper")});
    invalid = baseline;
    invalid.releaseHoldMs = 0;
    invalidConfigurations.append({invalid, QStringLiteral("hold")});

    for (const auto &[candidate, errorFragment] : invalidConfigurations) {
        QString error;
        QVERIFY(!settings.saveGainConfiguration(candidate, &error));
        QVERIFY2(error.contains(errorFragment, Qt::CaseInsensitive), qPrintable(error));
        QCOMPARE(settings.gainConfiguration(), baseline);
    }
}

void AppTest::rejectsCorruptedGainConfigurationValueTypes()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    const GainConfiguration defaults;
    GainConfiguration valid = defaults;
    valid.shifts[0] = 1;
    valid.automatic[0] = true;
    valid.upperPeakRatio = 0.75;
    const auto writeValidConfiguration = [&path, &valid]() {
        AppSettings settings(path);
        QString error;
        QVERIFY2(settings.saveGainConfiguration(valid, &error), qPrintable(error));
    };

    struct Corruption {
        QString name;
        std::function<void(QSettings &)> apply;
    };
    const QList<Corruption> corruptions {
        {QStringLiteral("boolean shift"), [](QSettings &raw) {
             QVariantList shifts = raw.value(QStringLiteral("gainConfiguration/shifts")).toList();
             shifts[0] = true;
             raw.setValue(QStringLiteral("gainConfiguration/shifts"), shifts);
         }},
        {QStringLiteral("floating shift"), [](QSettings &raw) {
             QVariantList shifts = raw.value(QStringLiteral("gainConfiguration/shifts")).toList();
             shifts[0] = 1.5;
             raw.setValue(QStringLiteral("gainConfiguration/shifts"), shifts);
         }},
        {QStringLiteral("invalid string shift"), [](QSettings &raw) {
             QVariantList shifts = raw.value(QStringLiteral("gainConfiguration/shifts")).toList();
             shifts[0] = QStringLiteral("1x");
             raw.setValue(QStringLiteral("gainConfiguration/shifts"), shifts);
         }},
        {QStringLiteral("boolean ratio"), [](QSettings &raw) {
             raw.setValue(QStringLiteral("gainConfiguration/upperPeakRatio"), true);
         }},
        {QStringLiteral("invalid string ratio"), [](QSettings &raw) {
             raw.setValue(QStringLiteral("gainConfiguration/lowerPeakRatio"), QStringLiteral("0.35x"));
         }},
        {QStringLiteral("floating hold"), [](QSettings &raw) {
             raw.setValue(QStringLiteral("gainConfiguration/releaseHoldMs"), 500.0);
         }},
        {QStringLiteral("string step"), [](QSettings &raw) {
             raw.setValue(QStringLiteral("gainConfiguration/releaseStepMs"), QStringLiteral("250x"));
         }},
    };

    for (const Corruption &corruption : corruptions) {
        {
            writeValidConfiguration();
        }
        {
            QSettings raw(path, QSettings::IniFormat);
            corruption.apply(raw);
            raw.sync();
            QCOMPARE(raw.status(), QSettings::NoError);
        }
        AppSettings settings(path);
        QVERIFY2(settings.gainConfiguration() == defaults, qPrintable(corruption.name));
    }
}

void AppTest::loadsCanonicalIniStringGainConfigurationAfterColdStart()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    GainConfiguration expected;
    expected.shifts[0] = -3;
    expected.automatic[0] = true;
    expected.upperPeakRatio = 1e-6;
    expected.lowerPeakRatio = 0.0;
    expected.releaseHoldMs = 500;
    expected.releaseStepMs = 250;
    {
        QSettings raw(path, QSettings::IniFormat);
        QVariantList shifts;
        QVariantList automatic;
        for (int channel = 0; channel < GainConfiguration::ChannelCount; ++channel) {
            shifts.append(channel == 0 ? QStringLiteral("-3") : QStringLiteral("0"));
            automatic.append(channel == 0);
        }
        raw.setValue(QStringLiteral("gainConfiguration/shifts"), shifts);
        raw.setValue(QStringLiteral("gainConfiguration/automatic"), automatic);
        raw.setValue(QStringLiteral("gainConfiguration/upperPeakRatio"), QStringLiteral("1e-06"));
        raw.setValue(QStringLiteral("gainConfiguration/lowerPeakRatio"), QStringLiteral("0"));
        raw.setValue(QStringLiteral("gainConfiguration/releaseHoldMs"), QStringLiteral("500"));
        raw.setValue(QStringLiteral("gainConfiguration/releaseStepMs"), QStringLiteral("250"));
        raw.sync();
        QCOMPARE(raw.status(), QSettings::NoError);
    }

    AppSettings reloaded(path);
    QCOMPARE(reloaded.gainConfiguration(), expected);
}

void AppTest::rejectsNonCanonicalIniStringGainConfigurationAfterColdStart()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    const GainConfiguration defaults;
    const QList<QPair<QString, QVariant>> corruptions {
        {QStringLiteral("gainConfiguration/shifts"), QVariantList(GainConfiguration::ChannelCount, QStringLiteral("01"))},
        {QStringLiteral("gainConfiguration/releaseHoldMs"), QStringLiteral("1.0")},
        {QStringLiteral("gainConfiguration/releaseStepMs"), QStringLiteral(" 250")},
        {QStringLiteral("gainConfiguration/upperPeakRatio"), QStringLiteral("NaN")},
        {QStringLiteral("gainConfiguration/lowerPeakRatio"), QStringLiteral("Inf")},
    };

    for (const auto &[key, value] : corruptions) {
        {
            AppSettings settings(path);
            QVERIFY(settings.saveGainConfiguration(GainConfiguration()));
        }
        {
            QSettings raw(path, QSettings::IniFormat);
            raw.setValue(key, value);
            raw.sync();
            QCOMPARE(raw.status(), QSettings::NoError);
        }
        AppSettings reloaded(path);
        QCOMPARE(reloaded.gainConfiguration(), defaults);
    }
}

void AppTest::restoresGainConfigurationAfterSyncFailure()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString activeDirectory = directory.filePath(QStringLiteral("active"));
    const QString movedDirectory = directory.filePath(QStringLiteral("moved"));
    QVERIFY(QDir().mkpath(activeDirectory));
    const QString path = QDir(activeDirectory).filePath(QStringLiteral("settings.ini"));
    GainConfiguration original;
    original.shifts[3] = -2;
    original.automatic[3] = true;
    {
        AppSettings writer(path);
        QVERIFY(writer.saveGainConfiguration(original));
    }

    {
        AppSettings settings(path);
        QCOMPARE(settings.gainConfiguration(), original);
        GainConfiguration replacement = original;
        replacement.shifts[3] = 4;
        replacement.automatic[3] = false;
        QVERIFY(QDir(directory.path()).rename(QStringLiteral("active"), QStringLiteral("moved")));

        QString error;
        QFile blockingEntry(activeDirectory);
        QVERIFY(blockingEntry.open(QIODevice::WriteOnly));
        blockingEntry.close();
        QVERIFY(!settings.saveGainConfiguration(replacement, &error));
        QVERIFY(error.contains(QStringLiteral("sync"), Qt::CaseInsensitive));
        QVERIFY(!error.contains(QStringLiteral("rollback"), Qt::CaseInsensitive));
        QCOMPARE(settings.gainConfiguration(), original);
        QVERIFY(blockingEntry.remove());
        QVERIFY(QDir(directory.path()).rename(QStringLiteral("moved"), QStringLiteral("active")));

        ConnectionSettings pendingConnection = settings.connection();
        pendingConnection.host = QStringLiteral("192.168.2.99");
        pendingConnection.port = 9977;
        pendingConnection.autoReconnect = false;
        settings.saveConnection(pendingConnection);
        QVERIFY2(settings.saveGainConfiguration(replacement, &error), qPrintable(error));
        QCOMPARE(settings.gainConfiguration(), replacement);
    }

    AppSettings reloaded(path);
    QCOMPARE(reloaded.gainConfiguration().shifts.at(3), 4);
    QCOMPARE(reloaded.gainConfiguration().automatic.at(3), false);
    QCOMPARE(reloaded.connection().host, QStringLiteral("192.168.2.99"));
    QCOMPARE(reloaded.connection().port, quint16(9977));
    QCOMPARE(reloaded.connection().autoReconnect, false);
}

void AppTest::persistsAudioAnalysisSettings()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    AudioAnalysisSettings expected;
    expected.lastWavePath = QStringLiteral("D:/capture.wav");
    expected.lastPhotoPath = QStringLiteral("D:/view.png");
    expected.outputChannel = 17;
    expected.volume = 0.6;
    expected.locatorId = QStringLiteral("gcc-phat-tdoa");
    expected.windowSamples = 4096;
    expected.updateIntervalMs = 100;
    expected.soundSpeedMetersPerSecond = 344.0;
    expected.smoothingFactor = 0.25;
    expected.confidenceThreshold = 0.55;
    expected.camera.centerAzimuthDegrees = 10.0;
    expected.camera.centerElevationDegrees = -5.0;
    expected.camera.horizontalFovDegrees = 100.0;
    expected.camera.verticalFovDegrees = 70.0;
    expected.camera.rollDegrees = 2.0;
    {
        AppSettings settings(path);
        settings.saveAudioAnalysis(expected);
    }

    AppSettings reloaded(path);
    QCOMPARE(reloaded.audioAnalysis(), expected);
}

void AppTest::invalidAudioAnalysisFieldsFallBackIndependently()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    {
        QSettings raw(path, QSettings::IniFormat);
        raw.setValue(QStringLiteral("audioAnalysis/lastWavePath"), QStringLiteral("D:/valid.wav"));
        raw.setValue(QStringLiteral("audioAnalysis/outputChannel"), 48);
        raw.setValue(QStringLiteral("audioAnalysis/volume"),
                     std::numeric_limits<double>::quiet_NaN());
        raw.setValue(QStringLiteral("audioAnalysis/locatorId"), QStringLiteral("   "));
        raw.setValue(QStringLiteral("audioAnalysis/windowSamples"), 1000);
        raw.setValue(QStringLiteral("audioAnalysis/updateIntervalMs"), 1);
        raw.setValue(QStringLiteral("audioAnalysis/soundSpeedMetersPerSecond"),
                     std::numeric_limits<double>::infinity());
        raw.setValue(QStringLiteral("audioAnalysis/smoothingFactor"), -1.0);
        raw.setValue(QStringLiteral("audioAnalysis/confidenceThreshold"), 2.0);
        raw.setValue(QStringLiteral("audioAnalysis/cameraCenterAzimuthDegrees"), 12.0);
        raw.setValue(QStringLiteral("audioAnalysis/cameraCenterElevationDegrees"), -4.0);
        raw.setValue(QStringLiteral("audioAnalysis/cameraHorizontalFovDegrees"), 180.0);
        raw.setValue(QStringLiteral("audioAnalysis/cameraVerticalFovDegrees"), 75.0);
        raw.setValue(QStringLiteral("audioAnalysis/cameraRollDegrees"), 1.0);
    }

    const AudioAnalysisSettings defaults;
    AppSettings settings(path);
    const AudioAnalysisSettings actual = settings.audioAnalysis();
    QCOMPARE(actual.lastWavePath, QStringLiteral("D:/valid.wav"));
    QCOMPARE(actual.outputChannel, defaults.outputChannel);
    QCOMPARE(actual.volume, defaults.volume);
    QCOMPARE(actual.locatorId, defaults.locatorId);
    QCOMPARE(actual.windowSamples, defaults.windowSamples);
    QCOMPARE(actual.updateIntervalMs, defaults.updateIntervalMs);
    QCOMPARE(actual.soundSpeedMetersPerSecond, defaults.soundSpeedMetersPerSecond);
    QCOMPARE(actual.smoothingFactor, defaults.smoothingFactor);
    QCOMPARE(actual.confidenceThreshold, defaults.confidenceThreshold);
    QCOMPARE(actual.camera.centerAzimuthDegrees, 12.0);
    QCOMPARE(actual.camera.centerElevationDegrees, -4.0);
    QCOMPARE(actual.camera.horizontalFovDegrees, defaults.camera.horizontalFovDegrees);
    QCOMPARE(actual.camera.verticalFovDegrees, 75.0);
    QCOMPARE(actual.camera.rollDegrees, 1.0);
}

void AppTest::persistsLiveLocalizationSettings()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    LiveLocalizationSettings expected;
    expected.enabled = true;
    expected.displayMode = QStringLiteral("heatmap");
    expected.heatmapOpacity = 0.55;
    expected.windowSamples = 4096;
    expected.updateIntervalMs = 120;
    expected.soundSpeedMetersPerSecond = 344.0;
    expected.smoothingFactor = 0.25;
    expected.confidenceThreshold = 0.6;
    expected.camera.centerAzimuthDegrees = 12.0;
    expected.camera.centerElevationDegrees = -4.0;
    expected.camera.horizontalFovDegrees = 82.0;
    expected.camera.verticalFovDegrees = 56.0;
    expected.camera.rollDegrees = 3.0;
    expected.calibrationRmsPixels = 2.5;
    expected.calibrationSamples = {
        {{-20.0, 10.0}, {0.1, 0.2}, 0.8, 1.2},
        {{20.0, 10.0}, {0.9, 0.2}, 0.82, 1.1},
        {{20.0, -10.0}, {0.9, 0.8}, 0.9, 1.0},
        {{-20.0, -10.0}, {0.1, 0.8}, 0.88, 1.3},
    };
    {
        AppSettings settings(path);
        settings.saveLiveLocalization(expected);
    }

    AppSettings reloaded(path);
    QCOMPARE(reloaded.liveLocalization(), expected);
}

void AppTest::invalidLiveLocalizationFieldsFallBackIndependently()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    {
        QSettings raw(path, QSettings::IniFormat);
        raw.setValue(QStringLiteral("liveLocalization/enabled"), true);
        raw.setValue(QStringLiteral("liveLocalization/displayMode"), QStringLiteral("glow"));
        raw.setValue(QStringLiteral("liveLocalization/heatmapOpacity"), 4.0);
        raw.setValue(QStringLiteral("liveLocalization/windowSamples"), 1000);
        raw.setValue(QStringLiteral("liveLocalization/updateIntervalMs"), 5);
        raw.setValue(QStringLiteral("liveLocalization/soundSpeedMetersPerSecond"), 344.0);
        raw.setValue(QStringLiteral("liveLocalization/smoothingFactor"), -1.0);
        raw.setValue(QStringLiteral("liveLocalization/confidenceThreshold"), 0.6);
        raw.setValue(QStringLiteral("liveLocalization/cameraCenterAzimuthDegrees"), 12.0);
        raw.setValue(QStringLiteral("liveLocalization/cameraHorizontalFovDegrees"), 180.0);
        raw.setValue(QStringLiteral("liveLocalization/calibrationVersion"), 999);
        raw.setValue(QStringLiteral("liveLocalization/calibrationCount"), 3);
    }

    const LiveLocalizationSettings defaults;
    AppSettings settings(path);
    const LiveLocalizationSettings actual = settings.liveLocalization();
    QVERIFY(actual.enabled);
    QCOMPARE(actual.displayMode, defaults.displayMode);
    QCOMPARE(actual.heatmapOpacity, defaults.heatmapOpacity);
    QCOMPARE(actual.windowSamples, defaults.windowSamples);
    QCOMPARE(actual.updateIntervalMs, defaults.updateIntervalMs);
    QCOMPARE(actual.soundSpeedMetersPerSecond, 344.0);
    QCOMPARE(actual.smoothingFactor, defaults.smoothingFactor);
    QCOMPARE(actual.confidenceThreshold, 0.6);
    QCOMPARE(actual.camera.centerAzimuthDegrees, 12.0);
    QCOMPARE(actual.camera.horizontalFovDegrees, defaults.camera.horizontalFovDegrees);
    QVERIFY(actual.calibrationSamples.isEmpty());
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
