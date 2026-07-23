#include "app/appsettings.h"

#include "control/controlvalues.h"

#include <QSettings>
#include <QRegularExpression>
#include <QFileInfo>
#include <QVariantList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace MicArray {

namespace {

bool parseCanonicalInteger(const QString &text, int *result)
{
    static const QRegularExpression pattern(QStringLiteral("^(?:0|-?[1-9][0-9]*)$"));
    if (!pattern.match(text).hasMatch()) {
        return false;
    }
    bool ok = false;
    const qint64 integer = text.toLongLong(&ok, 10);
    if (!ok || integer < std::numeric_limits<int>::min() || integer > std::numeric_limits<int>::max()) {
        return false;
    }
    *result = static_cast<int>(integer);
    return true;
}

bool parseCanonicalFiniteNumber(const QString &text, double *result)
{
    static const QRegularExpression pattern(
        QStringLiteral("^-?(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?$"));
    if (!pattern.match(text).hasMatch()) {
        return false;
    }
    bool ok = false;
    const double number = text.toDouble(&ok);
    if (!ok || !std::isfinite(number)) {
        return false;
    }
    *result = number;
    return true;
}

bool readIntegerSetting(const QVariant &value, int *result)
{
    qint64 integer = 0;
    switch (value.typeId()) {
    case QMetaType::Int:
        integer = value.toInt();
        break;
    case QMetaType::UInt:
        integer = value.toUInt();
        break;
    case QMetaType::LongLong:
        integer = value.toLongLong();
        break;
    case QMetaType::ULongLong: {
        const qulonglong unsignedInteger = value.toULongLong();
        if (unsignedInteger > static_cast<qulonglong>(std::numeric_limits<int>::max())) {
            return false;
        }
        integer = static_cast<qint64>(unsignedInteger);
        break;
    }
    case QMetaType::QString:
        return parseCanonicalInteger(value.toString(), result);
    default:
        return false;
    }
    if (integer < std::numeric_limits<int>::min() || integer > std::numeric_limits<int>::max()) {
        return false;
    }
    *result = static_cast<int>(integer);
    return true;
}

bool readFiniteNumberSetting(const QVariant &value, double *result)
{
    switch (value.typeId()) {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Float:
    case QMetaType::Double:
        break;
    case QMetaType::QString:
        return parseCanonicalFiniteNumber(value.toString(), result);
    default:
        return false;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number)) {
        return false;
    }
    *result = number;
    return true;
}

QString gainConfigurationSyncError(QSettings::Status status)
{
    switch (status) {
    case QSettings::AccessError:
        return QStringLiteral("Cannot sync gain configuration: access error");
    case QSettings::FormatError:
        return QStringLiteral("Cannot sync gain configuration: format error");
    case QSettings::NoError:
        break;
    }
    return QStringLiteral("Cannot sync gain configuration: unknown error");
}

bool isPowerOfTwo(int value)
{
    return value > 0 && (value & (value - 1)) == 0;
}

AudioAnalysisSettings sanitizeAudioAnalysisSettings(AudioAnalysisSettings value)
{
    const AudioAnalysisSettings defaults;
    if (value.outputChannel < 0 || value.outputChannel >= 48) {
        value.outputChannel = defaults.outputChannel;
    }
    if (!std::isfinite(value.volume) || value.volume < 0.0 || value.volume > 1.0) {
        value.volume = defaults.volume;
    }
    value.locatorId = value.locatorId.trimmed();
    if (value.locatorId.isEmpty()) {
        value.locatorId = defaults.locatorId;
    }
    if (value.windowSamples < 512 || value.windowSamples > 8192
        || !isPowerOfTwo(value.windowSamples)) {
        value.windowSamples = defaults.windowSamples;
    }
    if (value.updateIntervalMs < 20 || value.updateIntervalMs > 1000) {
        value.updateIntervalMs = defaults.updateIntervalMs;
    }
    if (!std::isfinite(value.soundSpeedMetersPerSecond)
        || value.soundSpeedMetersPerSecond < 300.0
        || value.soundSpeedMetersPerSecond > 380.0) {
        value.soundSpeedMetersPerSecond = defaults.soundSpeedMetersPerSecond;
    }
    if (!std::isfinite(value.smoothingFactor)
        || value.smoothingFactor < 0.0 || value.smoothingFactor > 1.0) {
        value.smoothingFactor = defaults.smoothingFactor;
    }
    if (!std::isfinite(value.confidenceThreshold)
        || value.confidenceThreshold < 0.0 || value.confidenceThreshold > 1.0) {
        value.confidenceThreshold = defaults.confidenceThreshold;
    }
    if (!std::isfinite(value.camera.centerAzimuthDegrees)
        || value.camera.centerAzimuthDegrees < -180.0
        || value.camera.centerAzimuthDegrees > 180.0) {
        value.camera.centerAzimuthDegrees = defaults.camera.centerAzimuthDegrees;
    }
    if (!std::isfinite(value.camera.centerElevationDegrees)
        || value.camera.centerElevationDegrees < -90.0
        || value.camera.centerElevationDegrees > 90.0) {
        value.camera.centerElevationDegrees = defaults.camera.centerElevationDegrees;
    }
    if (!std::isfinite(value.camera.horizontalFovDegrees)
        || value.camera.horizontalFovDegrees <= 1.0
        || value.camera.horizontalFovDegrees >= 179.0) {
        value.camera.horizontalFovDegrees = defaults.camera.horizontalFovDegrees;
    }
    if (!std::isfinite(value.camera.verticalFovDegrees)
        || value.camera.verticalFovDegrees <= 1.0
        || value.camera.verticalFovDegrees >= 179.0) {
        value.camera.verticalFovDegrees = defaults.camera.verticalFovDegrees;
    }
    if (!std::isfinite(value.camera.rollDegrees)
        || value.camera.rollDegrees < -180.0 || value.camera.rollDegrees > 180.0) {
        value.camera.rollDegrees = defaults.camera.rollDegrees;
    }
    return value;
}

LiveLocalizationSettings sanitizeLiveLocalizationSettings(LiveLocalizationSettings value)
{
    const LiveLocalizationSettings defaults;
    value.locatorId = value.locatorId.trimmed();
    if (value.locatorId.isEmpty()) {
        value.locatorId = defaults.locatorId;
    }
    value.displayMode = value.displayMode.trimmed().toLower();
    if (value.displayMode != QStringLiteral("marker")
        && value.displayMode != QStringLiteral("heatmap")) {
        value.displayMode = defaults.displayMode;
    }
    if (!std::isfinite(value.heatmapOpacity)
        || value.heatmapOpacity < 0.0 || value.heatmapOpacity > 1.0) {
        value.heatmapOpacity = defaults.heatmapOpacity;
    }
    if (value.windowSamples < 512 || value.windowSamples > 8192
        || !isPowerOfTwo(value.windowSamples)) {
        value.windowSamples = defaults.windowSamples;
    }
    if (value.updateIntervalMs < 20 || value.updateIntervalMs > 1000) {
        value.updateIntervalMs = defaults.updateIntervalMs;
    }
    if (!std::isfinite(value.soundSpeedMetersPerSecond)
        || value.soundSpeedMetersPerSecond < 300.0
        || value.soundSpeedMetersPerSecond > 380.0) {
        value.soundSpeedMetersPerSecond = defaults.soundSpeedMetersPerSecond;
    }
    if (!std::isfinite(value.smoothingFactor)
        || value.smoothingFactor < 0.0 || value.smoothingFactor > 1.0) {
        value.smoothingFactor = defaults.smoothingFactor;
    }
    if (!std::isfinite(value.confidenceThreshold)
        || value.confidenceThreshold < 0.0 || value.confidenceThreshold > 1.0) {
        value.confidenceThreshold = defaults.confidenceThreshold;
    }
    AudioAnalysisSettings cameraValidation;
    cameraValidation.camera = value.camera;
    value.camera = sanitizeAudioAnalysisSettings(cameraValidation).camera;
    if (!std::isfinite(value.calibrationRmsPixels) || value.calibrationRmsPixels < 0.0) {
        value.calibrationRmsPixels = defaults.calibrationRmsPixels;
    }
    bool calibrationValid = value.calibrationVersion == defaults.calibrationVersion
        && (value.calibrationSamples.isEmpty() || value.calibrationSamples.size() == 4);
    for (const VideoCalibrationSample &sample : std::as_const(value.calibrationSamples)) {
        calibrationValid = calibrationValid
            && std::isfinite(sample.direction.azimuthDegrees)
            && sample.direction.azimuthDegrees >= -180.0
            && sample.direction.azimuthDegrees <= 180.0
            && std::isfinite(sample.direction.elevationDegrees)
            && sample.direction.elevationDegrees >= -90.0
            && sample.direction.elevationDegrees <= 90.0
            && std::isfinite(sample.normalizedVideoPoint.x())
            && sample.normalizedVideoPoint.x() >= 0.0
            && sample.normalizedVideoPoint.x() <= 1.0
            && std::isfinite(sample.normalizedVideoPoint.y())
            && sample.normalizedVideoPoint.y() >= 0.0
            && sample.normalizedVideoPoint.y() <= 1.0
            && std::isfinite(sample.confidence)
            && sample.confidence >= 0.0 && sample.confidence <= 1.0
            && std::isfinite(sample.angularDeviationDegrees)
            && sample.angularDeviationDegrees >= 0.0;
    }
    if (!calibrationValid) {
        value.calibrationSamples.clear();
        value.calibrationRmsPixels = defaults.calibrationRmsPixels;
        value.calibrationVersion = defaults.calibrationVersion;
    }
    return value;
}

} // namespace

AppSettings::AppSettings(QObject *parent)
    : QObject(parent)
    , m_settings(std::make_unique<QSettings>())
{
    captureSettingsBackendIdentity();
    initializeSchema();
}

AppSettings::AppSettings(const QString &fileName, QObject *parent)
    : QObject(parent)
    , m_settingsBackend(SettingsBackend::IniFile)
    , m_settingsFileName(QFileInfo(fileName).absoluteFilePath())
    , m_settings(createSettingsBackend())
{
    captureSettingsBackendIdentity();
    initializeSchema();
}

AppSettings::~AppSettings() = default;

std::unique_ptr<QSettings> AppSettings::createSettingsBackend() const
{
    if (m_settingsBackend == SettingsBackend::IniFile) {
        auto settings = std::make_unique<QSettings>(m_settingsFileName, QSettings::IniFormat);
        settings->setFallbacksEnabled(m_fallbacksEnabled);
        settings->setAtomicSyncRequired(m_atomicSyncRequired);
        return settings;
    }
    auto settings = std::make_unique<QSettings>(static_cast<QSettings::Format>(m_settingsFormat),
                                                static_cast<QSettings::Scope>(m_settingsScope),
                                                m_settingsOrganization,
                                                m_settingsApplication);
    settings->setFallbacksEnabled(m_fallbacksEnabled);
    settings->setAtomicSyncRequired(m_atomicSyncRequired);
    return settings;
}

void AppSettings::captureSettingsBackendIdentity()
{
    m_settingsFormat = static_cast<int>(m_settings->format());
    m_settingsScope = static_cast<int>(m_settings->scope());
    m_settingsOrganization = m_settings->organizationName();
    m_settingsApplication = m_settings->applicationName();
    m_fallbacksEnabled = m_settings->fallbacksEnabled();
    m_atomicSyncRequired = m_settings->isAtomicSyncRequired();
}

void AppSettings::resetSettingsBackend()
{
    QList<QPair<QString, QVariant>> values;
    m_settings->setFallbacksEnabled(false);
    const QStringList keys = m_settings->allKeys();
    values.reserve(keys.size());
    for (const QString &key : keys) {
        values.append({key, m_settings->value(key)});
    }
    m_settings->setFallbacksEnabled(m_fallbacksEnabled);

    m_settings = createSettingsBackend();
    for (const auto &[key, value] : values) {
        m_settings->setValue(key, value);
    }
}

void AppSettings::initializeSchema()
{
    if (m_settings->value(QStringLiteral("schemaVersion"), 0).toInt() != SchemaVersion) {
        m_settings->setValue(QStringLiteral("schemaVersion"), SchemaVersion);
    }
}

ConnectionSettings AppSettings::connection() const
{
    ConnectionSettings value;
    const QString host = m_settings->value(QStringLiteral("connection/host"), value.host).toString().trimmed();
    const uint port = m_settings->value(QStringLiteral("connection/port"), value.port).toUInt();
    value.host = host.isEmpty() ? value.host : host;
    value.port = port >= 1 && port <= 65535 ? static_cast<quint16>(port) : value.port;
    value.autoReconnect = m_settings->value(QStringLiteral("connection/autoReconnect"), value.autoReconnect).toBool();
    return value;
}

AudioSettings AppSettings::audio() const
{
    AudioSettings value;
    const int sampleRate = m_settings->value(QStringLiteral("audio/sampleRate"), value.sampleRate).toInt();
    const int channelCount = m_settings->value(QStringLiteral("audio/channelCount"), value.channelCount).toInt();
    const int outputChannel = m_settings->value(QStringLiteral("audio/outputChannel"), value.outputChannel).toInt();
    const int averageWindowFrames = m_settings->value(QStringLiteral("audio/averageWindowFrames"), value.averageWindowFrames).toInt();
    value.sampleRate = sampleRate >= 8000 && sampleRate <= 192000 ? sampleRate : value.sampleRate;
    value.channelCount = channelCount >= 1 && channelCount <= 48 ? channelCount : value.channelCount;
    value.outputChannel = outputChannel >= 0 && outputChannel < value.channelCount ? outputChannel : value.outputChannel;
    value.averageWindowFrames = averageWindowFrames >= 1 && averageWindowFrames <= 10000 ? averageWindowFrames : value.averageWindowFrames;
    value.playbackEnabled = m_settings->value(QStringLiteral("audio/playbackEnabled"), value.playbackEnabled).toBool();
    return value;
}

VideoSettings AppSettings::video() const
{
    VideoSettings value;
    const QString format = m_settings->value(QStringLiteral("video/format"), value.format).toString().toUpper();
    value.format = format == QStringLiteral("JPG") || format == QStringLiteral("JPEG") ? QStringLiteral("JPG") : value.format;
    return value;
}

AudioAnalysisSettings AppSettings::audioAnalysis() const
{
    const AudioAnalysisSettings defaults;
    AudioAnalysisSettings value;
    value.lastWavePath = m_settings->value(QStringLiteral("audioAnalysis/lastWavePath"),
                                           defaults.lastWavePath).toString();
    value.lastPhotoPath = m_settings->value(QStringLiteral("audioAnalysis/lastPhotoPath"),
                                            defaults.lastPhotoPath).toString();
    if (!readIntegerSetting(m_settings->value(QStringLiteral("audioAnalysis/outputChannel"),
                                               defaults.outputChannel),
                            &value.outputChannel)) {
        value.outputChannel = defaults.outputChannel;
    }
    if (!readFiniteNumberSetting(m_settings->value(QStringLiteral("audioAnalysis/volume"),
                                                    defaults.volume),
                                 &value.volume)) {
        value.volume = defaults.volume;
    }
    value.locatorId = m_settings->value(QStringLiteral("audioAnalysis/locatorId"),
                                        defaults.locatorId).toString();
    if (!readIntegerSetting(m_settings->value(QStringLiteral("audioAnalysis/windowSamples"),
                                               defaults.windowSamples),
                            &value.windowSamples)) {
        value.windowSamples = defaults.windowSamples;
    }
    if (!readIntegerSetting(m_settings->value(QStringLiteral("audioAnalysis/updateIntervalMs"),
                                               defaults.updateIntervalMs),
                            &value.updateIntervalMs)) {
        value.updateIntervalMs = defaults.updateIntervalMs;
    }
    if (!readFiniteNumberSetting(
            m_settings->value(QStringLiteral("audioAnalysis/soundSpeedMetersPerSecond"),
                              defaults.soundSpeedMetersPerSecond),
            &value.soundSpeedMetersPerSecond)) {
        value.soundSpeedMetersPerSecond = defaults.soundSpeedMetersPerSecond;
    }
    if (!readFiniteNumberSetting(m_settings->value(QStringLiteral("audioAnalysis/smoothingFactor"),
                                                    defaults.smoothingFactor),
                                 &value.smoothingFactor)) {
        value.smoothingFactor = defaults.smoothingFactor;
    }
    if (!readFiniteNumberSetting(
            m_settings->value(QStringLiteral("audioAnalysis/confidenceThreshold"),
                              defaults.confidenceThreshold),
            &value.confidenceThreshold)) {
        value.confidenceThreshold = defaults.confidenceThreshold;
    }
    if (!readFiniteNumberSetting(
            m_settings->value(QStringLiteral("audioAnalysis/cameraCenterAzimuthDegrees"),
                              defaults.camera.centerAzimuthDegrees),
            &value.camera.centerAzimuthDegrees)) {
        value.camera.centerAzimuthDegrees = defaults.camera.centerAzimuthDegrees;
    }
    if (!readFiniteNumberSetting(
            m_settings->value(QStringLiteral("audioAnalysis/cameraCenterElevationDegrees"),
                              defaults.camera.centerElevationDegrees),
            &value.camera.centerElevationDegrees)) {
        value.camera.centerElevationDegrees = defaults.camera.centerElevationDegrees;
    }
    if (!readFiniteNumberSetting(
            m_settings->value(QStringLiteral("audioAnalysis/cameraHorizontalFovDegrees"),
                              defaults.camera.horizontalFovDegrees),
            &value.camera.horizontalFovDegrees)) {
        value.camera.horizontalFovDegrees = defaults.camera.horizontalFovDegrees;
    }
    if (!readFiniteNumberSetting(
            m_settings->value(QStringLiteral("audioAnalysis/cameraVerticalFovDegrees"),
                              defaults.camera.verticalFovDegrees),
            &value.camera.verticalFovDegrees)) {
        value.camera.verticalFovDegrees = defaults.camera.verticalFovDegrees;
    }
    if (!readFiniteNumberSetting(
            m_settings->value(QStringLiteral("audioAnalysis/cameraRollDegrees"),
                              defaults.camera.rollDegrees),
            &value.camera.rollDegrees)) {
        value.camera.rollDegrees = defaults.camera.rollDegrees;
    }
    return sanitizeAudioAnalysisSettings(value);
}

LiveLocalizationSettings AppSettings::liveLocalization() const
{
    const LiveLocalizationSettings defaults;
    LiveLocalizationSettings value;
    value.enabled = m_settings->value(QStringLiteral("liveLocalization/enabled"),
                                      defaults.enabled).toBool();
    value.locatorId = m_settings->value(QStringLiteral("liveLocalization/locatorId"),
                                        defaults.locatorId).toString();
    value.displayMode = m_settings->value(QStringLiteral("liveLocalization/displayMode"),
                                          defaults.displayMode).toString();
    readFiniteNumberSetting(
        m_settings->value(QStringLiteral("liveLocalization/heatmapOpacity"),
                          defaults.heatmapOpacity),
        &value.heatmapOpacity);
    readIntegerSetting(m_settings->value(QStringLiteral("liveLocalization/windowSamples"),
                                         defaults.windowSamples),
                       &value.windowSamples);
    readIntegerSetting(m_settings->value(QStringLiteral("liveLocalization/updateIntervalMs"),
                                         defaults.updateIntervalMs),
                       &value.updateIntervalMs);
    readFiniteNumberSetting(
        m_settings->value(QStringLiteral("liveLocalization/soundSpeedMetersPerSecond"),
                          defaults.soundSpeedMetersPerSecond),
        &value.soundSpeedMetersPerSecond);
    readFiniteNumberSetting(
        m_settings->value(QStringLiteral("liveLocalization/smoothingFactor"),
                          defaults.smoothingFactor),
        &value.smoothingFactor);
    readFiniteNumberSetting(
        m_settings->value(QStringLiteral("liveLocalization/confidenceThreshold"),
                          defaults.confidenceThreshold),
        &value.confidenceThreshold);
    readFiniteNumberSetting(
        m_settings->value(QStringLiteral("liveLocalization/cameraCenterAzimuthDegrees"),
                          defaults.camera.centerAzimuthDegrees),
        &value.camera.centerAzimuthDegrees);
    readFiniteNumberSetting(
        m_settings->value(QStringLiteral("liveLocalization/cameraCenterElevationDegrees"),
                          defaults.camera.centerElevationDegrees),
        &value.camera.centerElevationDegrees);
    readFiniteNumberSetting(
        m_settings->value(QStringLiteral("liveLocalization/cameraHorizontalFovDegrees"),
                          defaults.camera.horizontalFovDegrees),
        &value.camera.horizontalFovDegrees);
    readFiniteNumberSetting(
        m_settings->value(QStringLiteral("liveLocalization/cameraVerticalFovDegrees"),
                          defaults.camera.verticalFovDegrees),
        &value.camera.verticalFovDegrees);
    readFiniteNumberSetting(
        m_settings->value(QStringLiteral("liveLocalization/cameraRollDegrees"),
                          defaults.camera.rollDegrees),
        &value.camera.rollDegrees);
    readFiniteNumberSetting(
        m_settings->value(QStringLiteral("liveLocalization/calibrationRmsPixels"),
                          defaults.calibrationRmsPixels),
        &value.calibrationRmsPixels);
    readIntegerSetting(
        m_settings->value(QStringLiteral("liveLocalization/calibrationVersion"),
                          defaults.calibrationVersion),
        &value.calibrationVersion);
    int calibrationCount = 0;
    readIntegerSetting(
        m_settings->value(QStringLiteral("liveLocalization/calibrationCount"), 0),
        &calibrationCount);
    if (calibrationCount == 4) {
        value.calibrationSamples.reserve(4);
        for (int index = 0; index < calibrationCount; ++index) {
            const QString prefix = QStringLiteral("liveLocalization/calibration/%1/").arg(index);
            VideoCalibrationSample sample;
            readFiniteNumberSetting(m_settings->value(prefix + QStringLiteral("azimuth")),
                                    &sample.direction.azimuthDegrees);
            readFiniteNumberSetting(m_settings->value(prefix + QStringLiteral("elevation")),
                                    &sample.direction.elevationDegrees);
            double x = 0.0;
            double y = 0.0;
            readFiniteNumberSetting(m_settings->value(prefix + QStringLiteral("x")), &x);
            readFiniteNumberSetting(m_settings->value(prefix + QStringLiteral("y")), &y);
            sample.normalizedVideoPoint = {x, y};
            readFiniteNumberSetting(m_settings->value(prefix + QStringLiteral("confidence")),
                                    &sample.confidence);
            readFiniteNumberSetting(m_settings->value(prefix + QStringLiteral("deviation")),
                                    &sample.angularDeviationDegrees);
            value.calibrationSamples.append(sample);
        }
    } else if (calibrationCount != 0) {
        value.calibrationVersion = -1;
    }
    return sanitizeLiveLocalizationSettings(value);
}

GainConfiguration AppSettings::gainConfiguration() const
{
    const GainConfiguration defaults;
    const QString group = QStringLiteral("gainConfiguration/");
    const QString shiftsKey = group + QStringLiteral("shifts");
    const QString automaticKey = group + QStringLiteral("automatic");
    const QString upperPeakRatioKey = group + QStringLiteral("upperPeakRatio");
    const QString lowerPeakRatioKey = group + QStringLiteral("lowerPeakRatio");
    const QString releaseHoldMsKey = group + QStringLiteral("releaseHoldMs");
    const QString releaseStepMsKey = group + QStringLiteral("releaseStepMs");
    if (!m_settings->contains(shiftsKey)
        && !m_settings->contains(automaticKey)
        && !m_settings->contains(upperPeakRatioKey)
        && !m_settings->contains(lowerPeakRatioKey)
        && !m_settings->contains(releaseHoldMsKey)
        && !m_settings->contains(releaseStepMsKey)) {
        return defaults;
    }

    const QVariant shiftsValue = m_settings->value(shiftsKey);
    const QVariant automaticValue = m_settings->value(automaticKey);
    if (shiftsValue.typeId() != QMetaType::QVariantList
        || automaticValue.typeId() != QMetaType::QVariantList) {
        return defaults;
    }
    const QVariantList shifts = shiftsValue.toList();
    const QVariantList automatic = automaticValue.toList();
    if (shifts.size() != GainConfiguration::ChannelCount
        || automatic.size() != GainConfiguration::ChannelCount) {
        return defaults;
    }

    GainConfiguration candidate;
    for (int channel = 0; channel < GainConfiguration::ChannelCount; ++channel) {
        if (!readIntegerSetting(shifts.at(channel), &candidate.shifts[channel])
            || automatic.at(channel).typeId() != QMetaType::Bool) {
            return defaults;
        }
        candidate.automatic[channel] = automatic.at(channel).toBool();
    }
    if (!readFiniteNumberSetting(m_settings->value(upperPeakRatioKey), &candidate.upperPeakRatio)
        || !readFiniteNumberSetting(m_settings->value(lowerPeakRatioKey), &candidate.lowerPeakRatio)
        || !readIntegerSetting(m_settings->value(releaseHoldMsKey), &candidate.releaseHoldMs)
        || !readIntegerSetting(m_settings->value(releaseStepMsKey), &candidate.releaseStepMs)) {
        return defaults;
    }

    return GainConfiguration::validate(candidate) ? candidate : defaults;
}

void AppSettings::saveConnection(const ConnectionSettings &settingsValue)
{
    ConnectionSettings value = settingsValue;
    value.host = value.host.trimmed();
    if (value.host.isEmpty()) {
        value.host = ConnectionSettings().host;
    }
    if (value.port == 0) {
        value.port = ConnectionSettings().port;
    }
    m_settings->setValue(QStringLiteral("connection/host"), value.host);
    m_settings->setValue(QStringLiteral("connection/port"), value.port);
    m_settings->setValue(QStringLiteral("connection/autoReconnect"), value.autoReconnect);
}

void AppSettings::saveAudio(const AudioSettings &settingsValue)
{
    AudioSettings value = settingsValue;
    value.sampleRate = std::clamp(value.sampleRate, 8000, 192000);
    value.channelCount = std::clamp(value.channelCount, 1, 48);
    value.outputChannel = std::clamp(value.outputChannel, 0, value.channelCount - 1);
    value.averageWindowFrames = std::clamp(value.averageWindowFrames, 1, 10000);
    m_settings->setValue(QStringLiteral("audio/sampleRate"), value.sampleRate);
    m_settings->setValue(QStringLiteral("audio/channelCount"), value.channelCount);
    m_settings->setValue(QStringLiteral("audio/outputChannel"), value.outputChannel);
    m_settings->setValue(QStringLiteral("audio/averageWindowFrames"), value.averageWindowFrames);
    m_settings->setValue(QStringLiteral("audio/playbackEnabled"), value.playbackEnabled);
}

void AppSettings::saveVideo(const VideoSettings &settingsValue)
{
    const QString format = settingsValue.format.toUpper();
    m_settings->setValue(QStringLiteral("video/format"), format == QStringLiteral("JPEG") ? QStringLiteral("JPG") : format);
}

void AppSettings::saveAudioAnalysis(const AudioAnalysisSettings &settingsValue)
{
    const AudioAnalysisSettings value = sanitizeAudioAnalysisSettings(settingsValue);
    m_settings->setValue(QStringLiteral("audioAnalysis/lastWavePath"), value.lastWavePath);
    m_settings->setValue(QStringLiteral("audioAnalysis/lastPhotoPath"), value.lastPhotoPath);
    m_settings->setValue(QStringLiteral("audioAnalysis/outputChannel"), value.outputChannel);
    m_settings->setValue(QStringLiteral("audioAnalysis/volume"), value.volume);
    m_settings->setValue(QStringLiteral("audioAnalysis/locatorId"), value.locatorId);
    m_settings->setValue(QStringLiteral("audioAnalysis/windowSamples"), value.windowSamples);
    m_settings->setValue(QStringLiteral("audioAnalysis/updateIntervalMs"), value.updateIntervalMs);
    m_settings->setValue(QStringLiteral("audioAnalysis/soundSpeedMetersPerSecond"),
                         value.soundSpeedMetersPerSecond);
    m_settings->setValue(QStringLiteral("audioAnalysis/smoothingFactor"), value.smoothingFactor);
    m_settings->setValue(QStringLiteral("audioAnalysis/confidenceThreshold"),
                         value.confidenceThreshold);
    m_settings->setValue(QStringLiteral("audioAnalysis/cameraCenterAzimuthDegrees"),
                         value.camera.centerAzimuthDegrees);
    m_settings->setValue(QStringLiteral("audioAnalysis/cameraCenterElevationDegrees"),
                         value.camera.centerElevationDegrees);
    m_settings->setValue(QStringLiteral("audioAnalysis/cameraHorizontalFovDegrees"),
                         value.camera.horizontalFovDegrees);
    m_settings->setValue(QStringLiteral("audioAnalysis/cameraVerticalFovDegrees"),
                         value.camera.verticalFovDegrees);
    m_settings->setValue(QStringLiteral("audioAnalysis/cameraRollDegrees"),
                         value.camera.rollDegrees);
    m_settings->sync();
}

void AppSettings::saveLiveLocalization(const LiveLocalizationSettings &settingsValue)
{
    const LiveLocalizationSettings value = sanitizeLiveLocalizationSettings(settingsValue);
    const QString group = QStringLiteral("liveLocalization/");
    m_settings->setValue(group + QStringLiteral("enabled"), value.enabled);
    m_settings->setValue(group + QStringLiteral("locatorId"), value.locatorId);
    m_settings->setValue(group + QStringLiteral("displayMode"), value.displayMode);
    m_settings->setValue(group + QStringLiteral("heatmapOpacity"), value.heatmapOpacity);
    m_settings->setValue(group + QStringLiteral("windowSamples"), value.windowSamples);
    m_settings->setValue(group + QStringLiteral("updateIntervalMs"), value.updateIntervalMs);
    m_settings->setValue(group + QStringLiteral("soundSpeedMetersPerSecond"),
                         value.soundSpeedMetersPerSecond);
    m_settings->setValue(group + QStringLiteral("smoothingFactor"), value.smoothingFactor);
    m_settings->setValue(group + QStringLiteral("confidenceThreshold"),
                         value.confidenceThreshold);
    m_settings->setValue(group + QStringLiteral("cameraCenterAzimuthDegrees"),
                         value.camera.centerAzimuthDegrees);
    m_settings->setValue(group + QStringLiteral("cameraCenterElevationDegrees"),
                         value.camera.centerElevationDegrees);
    m_settings->setValue(group + QStringLiteral("cameraHorizontalFovDegrees"),
                         value.camera.horizontalFovDegrees);
    m_settings->setValue(group + QStringLiteral("cameraVerticalFovDegrees"),
                         value.camera.verticalFovDegrees);
    m_settings->setValue(group + QStringLiteral("cameraRollDegrees"),
                         value.camera.rollDegrees);
    m_settings->setValue(group + QStringLiteral("calibrationRmsPixels"),
                         value.calibrationRmsPixels);
    m_settings->setValue(group + QStringLiteral("calibrationVersion"),
                         value.calibrationVersion);
    m_settings->setValue(group + QStringLiteral("calibrationCount"),
                         value.calibrationSamples.size());
    m_settings->remove(group + QStringLiteral("calibration"));
    for (int index = 0; index < value.calibrationSamples.size(); ++index) {
        const QString prefix = group + QStringLiteral("calibration/%1/").arg(index);
        const VideoCalibrationSample &sample = value.calibrationSamples.at(index);
        m_settings->setValue(prefix + QStringLiteral("azimuth"),
                             sample.direction.azimuthDegrees);
        m_settings->setValue(prefix + QStringLiteral("elevation"),
                             sample.direction.elevationDegrees);
        m_settings->setValue(prefix + QStringLiteral("x"),
                             sample.normalizedVideoPoint.x());
        m_settings->setValue(prefix + QStringLiteral("y"),
                             sample.normalizedVideoPoint.y());
        m_settings->setValue(prefix + QStringLiteral("confidence"), sample.confidence);
        m_settings->setValue(prefix + QStringLiteral("deviation"),
                             sample.angularDeviationDegrees);
    }
    m_settings->sync();
}

bool AppSettings::saveGainConfiguration(const GainConfiguration &configuration, QString *error)
{
    QString validationError;
    if (!GainConfiguration::validate(configuration, &validationError)) {
        if (error) {
            *error = validationError;
        }
        return false;
    }

    if (m_settings->status() != QSettings::NoError) {
        resetSettingsBackend();
    }

    struct OriginalValue {
        QString key;
        bool exists = false;
        QVariant value;
    };
    const QList<OriginalValue> originalValues {
        {QStringLiteral("gainConfiguration/shifts"), m_settings->contains(QStringLiteral("gainConfiguration/shifts")), m_settings->value(QStringLiteral("gainConfiguration/shifts"))},
        {QStringLiteral("gainConfiguration/automatic"), m_settings->contains(QStringLiteral("gainConfiguration/automatic")), m_settings->value(QStringLiteral("gainConfiguration/automatic"))},
        {QStringLiteral("gainConfiguration/upperPeakRatio"), m_settings->contains(QStringLiteral("gainConfiguration/upperPeakRatio")), m_settings->value(QStringLiteral("gainConfiguration/upperPeakRatio"))},
        {QStringLiteral("gainConfiguration/lowerPeakRatio"), m_settings->contains(QStringLiteral("gainConfiguration/lowerPeakRatio")), m_settings->value(QStringLiteral("gainConfiguration/lowerPeakRatio"))},
        {QStringLiteral("gainConfiguration/releaseHoldMs"), m_settings->contains(QStringLiteral("gainConfiguration/releaseHoldMs")), m_settings->value(QStringLiteral("gainConfiguration/releaseHoldMs"))},
        {QStringLiteral("gainConfiguration/releaseStepMs"), m_settings->contains(QStringLiteral("gainConfiguration/releaseStepMs")), m_settings->value(QStringLiteral("gainConfiguration/releaseStepMs"))},
    };

    QVariantList shifts;
    QVariantList automatic;
    for (int channel = 0; channel < GainConfiguration::ChannelCount; ++channel) {
        shifts.append(configuration.shifts.at(channel));
        automatic.append(configuration.automatic.at(channel));
    }
    m_settings->setValue(QStringLiteral("gainConfiguration/shifts"), shifts);
    m_settings->setValue(QStringLiteral("gainConfiguration/automatic"), automatic);
    m_settings->setValue(QStringLiteral("gainConfiguration/upperPeakRatio"), configuration.upperPeakRatio);
    m_settings->setValue(QStringLiteral("gainConfiguration/lowerPeakRatio"), configuration.lowerPeakRatio);
    m_settings->setValue(QStringLiteral("gainConfiguration/releaseHoldMs"), configuration.releaseHoldMs);
    m_settings->setValue(QStringLiteral("gainConfiguration/releaseStepMs"), configuration.releaseStepMs);
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        const QSettings::Status status = m_settings->status();
        for (const OriginalValue &original : originalValues) {
            if (original.exists) {
                m_settings->setValue(original.key, original.value);
            } else {
                m_settings->remove(original.key);
            }
        }
        if (error) {
            *error = gainConfigurationSyncError(status);
        }
        return false;
    }
    if (error) {
        error->clear();
    }
    return true;
}

QList<ControlPreset> AppSettings::controlPresets() const
{
    QList<ControlPreset> presets;
    const int size = m_settings->beginReadArray(QStringLiteral("controlPresets"));
    for (int index = 0; index < size; ++index) {
        m_settings->setArrayIndex(index);
        ControlPreset preset;
        preset.name = m_settings->value(QStringLiteral("name")).toString().trimmed();
        const QVariantList storedValues = m_settings->value(QStringLiteral("values")).toList();
        for (const QVariant &stored : storedValues) {
            preset.values.append(static_cast<qint16>(stored.toInt()));
        }
        if (preset.values.size() == 8) {
            preset.values.resize(ControlValues::ChannelCount);
        }
        if (!preset.name.isEmpty() && preset.values.size() == ControlValues::ChannelCount) {
            presets.append(preset);
        }
    }
    m_settings->endArray();
    return presets;
}

bool AppSettings::saveControlPreset(const ControlPreset &presetValue)
{
    ControlPreset preset = presetValue;
    preset.name = preset.name.trimmed();
    if (preset.name.isEmpty() || preset.name.size() > 32 || preset.values.size() != ControlValues::ChannelCount) {
        return false;
    }

    QList<ControlPreset> presets = controlPresets();
    const auto existing = std::find_if(presets.begin(), presets.end(), [&preset](const ControlPreset &item) {
        return item.name == preset.name;
    });
    if (existing == presets.end()) {
        presets.append(preset);
    } else {
        *existing = preset;
    }
    writeControlPresets(presets);
    return true;
}

bool AppSettings::removeControlPreset(const QString &nameValue)
{
    const QString name = nameValue.trimmed();
    QList<ControlPreset> presets = controlPresets();
    const qsizetype removed = presets.removeIf([&name](const ControlPreset &preset) {
        return preset.name == name;
    });
    if (removed == 0) {
        return false;
    }
    writeControlPresets(presets);
    return true;
}

void AppSettings::writeControlPresets(const QList<ControlPreset> &presets)
{
    m_settings->remove(QStringLiteral("controlPresets"));
    m_settings->beginWriteArray(QStringLiteral("controlPresets"));
    for (int index = 0; index < presets.size(); ++index) {
        m_settings->setArrayIndex(index);
        m_settings->setValue(QStringLiteral("name"), presets.at(index).name);
        QVariantList storedValues;
        for (qint16 value : presets.at(index).values) {
            storedValues.append(value);
        }
        m_settings->setValue(QStringLiteral("values"), storedValues);
    }
    m_settings->endArray();
    m_settings->sync();
}

void AppSettings::resetAll()
{
    m_settings->clear();
    m_settings->setValue(QStringLiteral("schemaVersion"), SchemaVersion);
    m_settings->sync();
}

} // namespace MicArray
