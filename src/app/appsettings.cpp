#include "app/appsettings.h"

#include "control/controlvalues.h"
#include "control/directioncontrolmapper.h"

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

QString spatialResolutionModeKey(SpatialResolutionMode mode)
{
    switch (mode) {
    case SpatialResolutionMode::RealtimeEnhanced:
        return QStringLiteral("realtime-enhanced");
    case SpatialResolutionMode::HighPrecision:
        return QStringLiteral("high-precision");
    }
    return QStringLiteral("realtime-enhanced");
}

bool spatialResolutionModeFromKey(const QString &key,
                                  SpatialResolutionMode *mode)
{
    const QString normalized = key.trimmed().toLower();
    if (normalized == QStringLiteral("realtime-enhanced")) {
        *mode = SpatialResolutionMode::RealtimeEnhanced;
        return true;
    }
    if (normalized == QStringLiteral("high-precision")) {
        *mode = SpatialResolutionMode::HighPrecision;
        return true;
    }
    return false;
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
    if (!std::isfinite(
            value.neuralPeakEnhancementStrength)
        || value.neuralPeakEnhancementStrength < 0.0
        || value.neuralPeakEnhancementStrength > 1.0) {
        value.neuralPeakEnhancementStrength =
            defaults.neuralPeakEnhancementStrength;
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
    if (!std::isfinite(value.heatmapFocusExponent)
        || value.heatmapFocusExponent < 0.5
        || value.heatmapFocusExponent > 4.0) {
        value.heatmapFocusExponent = defaults.heatmapFocusExponent;
    }
    if (value.heatmapGridColumns < 16 || value.heatmapGridColumns > 640) {
        value.heatmapGridColumns = defaults.heatmapGridColumns;
    }
    if (value.heatmapGridRows < 9 || value.heatmapGridRows > 360) {
        value.heatmapGridRows = defaults.heatmapGridRows;
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
    if (!std::isfinite(
            value.neuralPeakEnhancementStrength)
        || value.neuralPeakEnhancementStrength < 0.0
        || value.neuralPeakEnhancementStrength > 1.0) {
        value.neuralPeakEnhancementStrength =
            defaults.neuralPeakEnhancementStrength;
    }
    AudioAnalysisSettings cameraValidation;
    cameraValidation.camera = value.camera;
    value.camera = sanitizeAudioAnalysisSettings(cameraValidation).camera;
    const QSize referenceSize = value.calibrationReferenceVideoSize;
    if ((referenceSize.width() == 0) != (referenceSize.height() == 0)
        || referenceSize.width() < 0 || referenceSize.height() < 0
        || referenceSize.width() > 65535 || referenceSize.height() > 65535) {
        value.calibrationReferenceVideoSize = defaults.calibrationReferenceVideoSize;
    }
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

bool validNoiseSource(NoiseSignalSource source)
{
    switch (source) {
    case NoiseSignalSource::FirstElement:
    case NoiseSignalSource::EnhancedA:
    case NoiseSignalSource::SuppressionB1:
    case NoiseSignalSource::SuppressionB2:
    case NoiseSignalSource::Denoised:
        return true;
    }
    return false;
}

QString noiseModeKey(NoiseReductionMode mode)
{
    return mode == NoiseReductionMode::TwoPoint
        ? QStringLiteral("twoPoint") : QStringLiteral("singlePoint");
}

QString noiseAlgorithmKey(NoiseReductionAlgorithm algorithm)
{
    return algorithm == NoiseReductionAlgorithm::AdaptiveWiener
        ? QStringLiteral("adaptive") : QStringLiteral("fixed");
}

NoiseReductionAlgorithm noiseAlgorithmFromKey(const QString &key)
{
    if (key == QStringLiteral("adaptive")) {
        return NoiseReductionAlgorithm::AdaptiveWiener;
    }
    return NoiseReductionAlgorithm::FixedParameterWiener;
}

QString bandPassPresetKey(BandPassPreset preset)
{
    switch (preset) {
    case BandPassPreset::Voice:
        return QStringLiteral("voice");
    case BandPassPreset::Music:
        return QStringLiteral("music");
    case BandPassPreset::Custom:
        return QStringLiteral("custom");
    }
    return QStringLiteral("voice");
}

BandPassPreset bandPassPresetFromKey(const QString &key)
{
    if (key == QStringLiteral("music")) {
        return BandPassPreset::Music;
    }
    if (key == QStringLiteral("custom")) {
        return BandPassPreset::Custom;
    }
    return BandPassPreset::Voice;
}

QString postProcessingNodeKey(NoisePostProcessingNode node)
{
    switch (node) {
    case NoisePostProcessingNode::BandPass:
        return QStringLiteral("bandPass");
    case NoisePostProcessingNode::LogMmse:
        return QStringLiteral("logMmse");
    case NoisePostProcessingNode::AutomaticGainControl:
        return QStringLiteral("agc");
    }
    return {};
}

bool postProcessingNodeFromKey(
    const QString &key, NoisePostProcessingNode *node)
{
    if (key == QStringLiteral("bandPass")) {
        *node = NoisePostProcessingNode::BandPass;
        return true;
    }
    if (key == QStringLiteral("logMmse")) {
        *node = NoisePostProcessingNode::LogMmse;
        return true;
    }
    if (key == QStringLiteral("agc")) {
        *node = NoisePostProcessingNode::AutomaticGainControl;
        return true;
    }
    return false;
}

NoiseReductionMode noiseModeFromKey(const QString &key)
{
    if (key == QStringLiteral("twoPoint")) {
        return NoiseReductionMode::TwoPoint;
    }
    return NoiseReductionMode::SinglePoint;
}

QString noiseSourceKey(NoiseSignalSource source)
{
    switch (source) {
    case NoiseSignalSource::FirstElement:
        return QStringLiteral("firstElement");
    case NoiseSignalSource::EnhancedA:
        return QStringLiteral("enhancedA");
    case NoiseSignalSource::SuppressionB1:
        return QStringLiteral("suppressionB1");
    case NoiseSignalSource::SuppressionB2:
        return QStringLiteral("suppressionB2");
    case NoiseSignalSource::Denoised:
        return QStringLiteral("denoised");
    }
    return QStringLiteral("denoised");
}

NoiseSignalSource noiseSourceFromKey(
    const QString &key, NoiseSignalSource fallback)
{
    if (key == QStringLiteral("firstElement")) {
        return NoiseSignalSource::FirstElement;
    }
    if (key == QStringLiteral("enhancedA")) {
        return NoiseSignalSource::EnhancedA;
    }
    if (key == QStringLiteral("suppressionB1")) {
        return NoiseSignalSource::SuppressionB1;
    }
    if (key == QStringLiteral("suppressionB2")) {
        return NoiseSignalSource::SuppressionB2;
    }
    if (key == QStringLiteral("denoised")) {
        return NoiseSignalSource::Denoised;
    }
    return fallback;
}

NoiseReductionSettings sanitizeNoiseReductionSettings(
    NoiseReductionSettings value)
{
    const NoiseReductionSettings defaults;
    if (value.algorithm != NoiseReductionAlgorithm::FixedParameterWiener
        && value.algorithm != NoiseReductionAlgorithm::AdaptiveWiener) {
        value.algorithm = defaults.algorithm;
    }
    if (value.mode != NoiseReductionMode::SinglePoint
        && value.mode != NoiseReductionMode::TwoPoint) {
        value.mode = defaults.mode;
    }
    auto sanitizeCoefficient = [](double value, double fallback) {
        return std::isfinite(value) && value >= 0.0 && value <= 100.0
            ? value : fallback;
    };
    value.singleSuppressionCoefficient = sanitizeCoefficient(
        value.singleSuppressionCoefficient,
        defaults.singleSuppressionCoefficient);
    value.dualB1SuppressionCoefficient = sanitizeCoefficient(
        value.dualB1SuppressionCoefficient,
        defaults.dualB1SuppressionCoefficient);
    value.dualB2SuppressionCoefficient = sanitizeCoefficient(
        value.dualB2SuppressionCoefficient,
        defaults.dualB2SuppressionCoefficient);
    if (!validNoiseSource(value.monitorSource)) {
        value.monitorSource = defaults.monitorSource;
    }
    if (!validNoiseSource(value.firstPlotSource)) {
        value.firstPlotSource = defaults.firstPlotSource;
    }
    if (!validNoiseSource(value.secondPlotSource)) {
        value.secondPlotSource = defaults.secondPlotSource;
    }
    if (!std::isfinite(value.monitorGainDb)
        || value.monitorGainDb < -60.0 || value.monitorGainDb > 48.0) {
        value.monitorGainDb = defaults.monitorGainDb;
    }
    if (value.monitorBufferMilliseconds < 50
        || value.monitorBufferMilliseconds > 1000) {
        value.monitorBufferMilliseconds = defaults.monitorBufferMilliseconds;
    }
    if (value.bandPass.preset != BandPassPreset::Voice
        && value.bandPass.preset != BandPassPreset::Music
        && value.bandPass.preset != BandPassPreset::Custom) {
        value.bandPass.preset = defaults.bandPass.preset;
    }
    if (!std::isfinite(value.bandPass.lowCutoffHz)
        || !std::isfinite(value.bandPass.highCutoffHz)
        || value.bandPass.lowCutoffHz <= 0.0
        || value.bandPass.lowCutoffHz >= value.bandPass.highCutoffHz) {
        value.bandPass.lowCutoffHz = defaults.bandPass.lowCutoffHz;
        value.bandPass.highCutoffHz = defaults.bandPass.highCutoffHz;
    }
    value.postProcessingOrder =
        normalizedPostProcessingOrder(value.postProcessingOrder);
    const bool validFftSize =
        value.logMmse.fftSize >= 256
        && value.logMmse.fftSize <= 2048
        && (value.logMmse.fftSize
            & (value.logMmse.fftSize - 1)) == 0;
    if (!validFftSize) {
        value.logMmse.fftSize = defaults.logMmse.fftSize;
    }
    auto validUnitInterval = [](double candidate) {
        return std::isfinite(candidate)
            && candidate >= 0.0 && candidate < 1.0;
    };
    if (!validUnitInterval(value.logMmse.overlapRatio)) {
        value.logMmse.overlapRatio = defaults.logMmse.overlapRatio;
    }
    if (!validUnitInterval(value.logMmse.priorSnrSmoothing)) {
        value.logMmse.priorSnrSmoothing =
            defaults.logMmse.priorSnrSmoothing;
    }
    if (!validUnitInterval(value.logMmse.noisePowerSmoothing)) {
        value.logMmse.noisePowerSmoothing =
            defaults.logMmse.noisePowerSmoothing;
    }
    if (!std::isfinite(value.logMmse.minimumGainDb)
        || value.logMmse.minimumGainDb < -80.0
        || value.logMmse.minimumGainDb > 0.0) {
        value.logMmse.minimumGainDb =
            defaults.logMmse.minimumGainDb;
    }
    auto sanitizeRange = [](double candidate,
                            double minimum,
                            double maximum,
                            double fallback) {
        return std::isfinite(candidate)
                && candidate >= minimum && candidate <= maximum
            ? candidate : fallback;
    };
    value.automaticGainControl.targetLevelDbfs = sanitizeRange(
        value.automaticGainControl.targetLevelDbfs,
        -60.0, 0.0,
        defaults.automaticGainControl.targetLevelDbfs);
    value.automaticGainControl.maximumGainDb = sanitizeRange(
        value.automaticGainControl.maximumGainDb,
        0.0, 60.0,
        defaults.automaticGainControl.maximumGainDb);
    value.automaticGainControl.maximumAttenuationDb = sanitizeRange(
        value.automaticGainControl.maximumAttenuationDb,
        -60.0, 0.0,
        defaults.automaticGainControl.maximumAttenuationDb);
    value.automaticGainControl.attackMilliseconds = sanitizeRange(
        value.automaticGainControl.attackMilliseconds,
        1.0, 1000.0,
        defaults.automaticGainControl.attackMilliseconds);
    value.automaticGainControl.releaseMilliseconds = sanitizeRange(
        value.automaticGainControl.releaseMilliseconds,
        1.0, 5000.0,
        defaults.automaticGainControl.releaseMilliseconds);
    value.automaticGainControl.limiterLevelDbfs = sanitizeRange(
        value.automaticGainControl.limiterLevelDbfs,
        -20.0, 0.0,
        defaults.automaticGainControl.limiterLevelDbfs);
    if (value.controlTabIndex < 0 || value.controlTabIndex > 2) {
        value.controlTabIndex = defaults.controlTabIndex;
    }
    if (value.selectedDataFlowNode < -1
        || value.selectedDataFlowNode
               > static_cast<int>(
                   NoisePostProcessingNode::AutomaticGainControl)) {
        value.selectedDataFlowNode =
            defaults.selectedDataFlowNode;
    }
    if (!value.mapping.isValid()) {
        value.mapping = defaults.mapping;
    }
    value.enhancedA = DirectionControlMapper::clampDirection(
        value.enhancedA, value.mapping);
    value.suppressionB1 = DirectionControlMapper::clampDirection(
        value.suppressionB1, value.mapping);
    value.suppressionB2 = DirectionControlMapper::clampDirection(
        value.suppressionB2, value.mapping);
    if (!std::isfinite(value.camera.centerAzimuthDegrees)
        || value.camera.centerAzimuthDegrees < -180.0
        || value.camera.centerAzimuthDegrees > 180.0) {
        value.camera.centerAzimuthDegrees =
            defaults.camera.centerAzimuthDegrees;
    }
    if (!std::isfinite(value.camera.centerElevationDegrees)
        || value.camera.centerElevationDegrees < -90.0
        || value.camera.centerElevationDegrees > 90.0) {
        value.camera.centerElevationDegrees =
            defaults.camera.centerElevationDegrees;
    }
    if (!std::isfinite(value.camera.horizontalFovDegrees)
        || value.camera.horizontalFovDegrees <= 1.0
        || value.camera.horizontalFovDegrees > 180.0) {
        value.camera.horizontalFovDegrees =
            defaults.camera.horizontalFovDegrees;
    }
    if (!std::isfinite(value.camera.verticalFovDegrees)
        || value.camera.verticalFovDegrees <= 1.0
        || value.camera.verticalFovDegrees > 180.0) {
        value.camera.verticalFovDegrees =
            defaults.camera.verticalFovDegrees;
    }
    if (!std::isfinite(value.camera.rollDegrees)
        || value.camera.rollDegrees < -180.0
        || value.camera.rollDegrees > 180.0) {
        value.camera.rollDegrees = defaults.camera.rollDegrees;
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

AppearanceSettings AppSettings::appearance() const
{
    AppearanceSettings value;
    const QString theme = m_settings->value(QStringLiteral("appearance/theme"),
                                             value.theme).toString().trimmed().toLower();
    if (theme == QStringLiteral("light") || theme == QStringLiteral("dark")) {
        value.theme = theme;
    }
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
    value.neuralPeakEnhancementEnabled =
        m_settings->value(
            QStringLiteral(
                "audioAnalysis/neuralPeakEnhancementEnabled"),
            defaults.neuralPeakEnhancementEnabled).toBool();
    if (!readFiniteNumberSetting(
            m_settings->value(
                QStringLiteral(
                    "audioAnalysis/neuralPeakEnhancementStrength"),
                defaults.neuralPeakEnhancementStrength),
            &value.neuralPeakEnhancementStrength)) {
        value.neuralPeakEnhancementStrength =
            defaults.neuralPeakEnhancementStrength;
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
    if (!spatialResolutionModeFromKey(
            m_settings->value(
                QStringLiteral("liveLocalization/spatialResolutionMode"),
                spatialResolutionModeKey(defaults.spatialResolutionMode)).toString(),
            &value.spatialResolutionMode)) {
        value.spatialResolutionMode = defaults.spatialResolutionMode;
    }
    readFiniteNumberSetting(
        m_settings->value(QStringLiteral("liveLocalization/heatmapOpacity"),
                          defaults.heatmapOpacity),
        &value.heatmapOpacity);
    readFiniteNumberSetting(
        m_settings->value(
            QStringLiteral("liveLocalization/heatmapFocusExponent"),
            defaults.heatmapFocusExponent),
        &value.heatmapFocusExponent);
    readIntegerSetting(
        m_settings->value(QStringLiteral("liveLocalization/heatmapGridColumns"),
                          defaults.heatmapGridColumns),
        &value.heatmapGridColumns);
    readIntegerSetting(
        m_settings->value(QStringLiteral("liveLocalization/heatmapGridRows"),
                          defaults.heatmapGridRows),
        &value.heatmapGridRows);
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
    const QString trustedSourceThresholdKey = QStringLiteral(
        "liveLocalization/trustedSourceThreshold");
    const QString legacyConfidenceThresholdKey = QStringLiteral(
        "liveLocalization/confidenceThreshold");
    if (m_settings->contains(trustedSourceThresholdKey)) {
        if (!readFiniteNumberSetting(
                m_settings->value(trustedSourceThresholdKey),
                &value.confidenceThreshold)) {
            value.confidenceThreshold =
                defaults.confidenceThreshold;
        }
    } else if (m_settings->contains(
                   legacyConfidenceThresholdKey)) {
        double legacyThreshold = defaults.confidenceThreshold;
        if (readFiniteNumberSetting(
                m_settings->value(
                    legacyConfidenceThresholdKey),
                &legacyThreshold)
            && legacyThreshold != 0.45) {
            value.confidenceThreshold = legacyThreshold;
        }
    }
    value.neuralPeakEnhancementEnabled =
        m_settings->value(
            QStringLiteral(
                "liveLocalization/neuralPeakEnhancementEnabled"),
            defaults.neuralPeakEnhancementEnabled).toBool();
    readFiniteNumberSetting(
        m_settings->value(
            QStringLiteral(
                "liveLocalization/neuralPeakEnhancementStrength"),
            defaults.neuralPeakEnhancementStrength),
        &value.neuralPeakEnhancementStrength);
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
    value.showCalibrationOverlay =
        m_settings->value(QStringLiteral("liveLocalization/showCalibrationOverlay"),
                          defaults.showCalibrationOverlay).toBool();
    int referenceWidth = defaults.calibrationReferenceVideoSize.width();
    int referenceHeight = defaults.calibrationReferenceVideoSize.height();
    readIntegerSetting(
        m_settings->value(QStringLiteral("liveLocalization/calibrationReferenceVideoWidth"),
                          referenceWidth),
        &referenceWidth);
    readIntegerSetting(
        m_settings->value(QStringLiteral("liveLocalization/calibrationReferenceVideoHeight"),
                          referenceHeight),
        &referenceHeight);
    value.calibrationReferenceVideoSize = {referenceWidth, referenceHeight};
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

NoiseReductionSettings AppSettings::noiseReduction() const
{
    const NoiseReductionSettings defaults;
    NoiseReductionSettings value;
    const QString group = QStringLiteral("noiseReduction/");
    value.enabled =
        m_settings->value(group + QStringLiteral("enabled"), defaults.enabled)
            .toBool();
    const QString algorithm =
        m_settings->value(group + QStringLiteral("algorithm"),
                          noiseAlgorithmKey(defaults.algorithm)).toString();
    value.algorithm = noiseAlgorithmFromKey(algorithm);
    const QString mode =
        m_settings->value(group + QStringLiteral("mode"),
                          noiseModeKey(defaults.mode)).toString();
    value.mode = noiseModeFromKey(mode);
    value.monitorEnabled =
        m_settings->value(group + QStringLiteral("monitorEnabled"),
                          defaults.monitorEnabled).toBool();
    if (!readIntegerSetting(
            m_settings->value(
                group + QStringLiteral("monitorBufferMilliseconds"),
                defaults.monitorBufferMilliseconds),
            &value.monitorBufferMilliseconds)) {
        value.monitorBufferMilliseconds = defaults.monitorBufferMilliseconds;
    }
    value.monitorSource = noiseSourceFromKey(
        m_settings->value(group + QStringLiteral("monitorSource"),
                          noiseSourceKey(defaults.monitorSource)).toString(),
        defaults.monitorSource);
    value.firstPlotSource = noiseSourceFromKey(
        m_settings->value(group + QStringLiteral("firstPlotSource"),
                          noiseSourceKey(defaults.firstPlotSource)).toString(),
        defaults.firstPlotSource);
    value.secondPlotSource = noiseSourceFromKey(
        m_settings->value(group + QStringLiteral("secondPlotSource"),
                          noiseSourceKey(defaults.secondPlotSource)).toString(),
        defaults.secondPlotSource);
    value.showMarkers =
        m_settings->value(group + QStringLiteral("showMarkers"),
                          defaults.showMarkers).toBool();
    value.bandPass.enabled =
        m_settings->value(group + QStringLiteral("bandPassEnabled"),
                          defaults.bandPass.enabled).toBool();
    value.bandPass.preset = bandPassPresetFromKey(
        m_settings->value(group + QStringLiteral("bandPassPreset"),
                          bandPassPresetKey(defaults.bandPass.preset))
            .toString());
    value.logMmse.enabled =
        m_settings->value(group + QStringLiteral("logMmseEnabled"),
                          defaults.logMmse.enabled).toBool();
    value.automaticGainControl.enabled =
        m_settings->value(group + QStringLiteral("agcEnabled"),
                          defaults.automaticGainControl.enabled).toBool();
    const QStringList orderKeys =
        m_settings->value(
            group + QStringLiteral("postProcessingOrder"),
            QStringList({
                postProcessingNodeKey(
                    NoisePostProcessingNode::BandPass),
                postProcessingNodeKey(
                    NoisePostProcessingNode::LogMmse),
                postProcessingNodeKey(
                    NoisePostProcessingNode::AutomaticGainControl),
            })).toStringList();
    value.postProcessingOrder.clear();
    for (const QString &key : orderKeys) {
        NoisePostProcessingNode node;
        if (postProcessingNodeFromKey(key, &node)) {
            value.postProcessingOrder.append(node);
        }
    }
    if (!readIntegerSetting(
            m_settings->value(
                group + QStringLiteral("controlTabIndex"),
                defaults.controlTabIndex),
            &value.controlTabIndex)) {
        value.controlTabIndex = defaults.controlTabIndex;
    }
    if (!readIntegerSetting(
            m_settings->value(
                group + QStringLiteral("selectedDataFlowNode"),
                defaults.selectedDataFlowNode),
            &value.selectedDataFlowNode)) {
        value.selectedDataFlowNode =
            defaults.selectedDataFlowNode;
    }

    auto readDouble = [this, &group](const QString &key,
                                     double fallback,
                                     double *target) {
        if (!readFiniteNumberSetting(
                m_settings->value(group + key, fallback), target)) {
            *target = fallback;
        }
    };
    readDouble(QStringLiteral("monitorGainDb"),
               defaults.monitorGainDb,
               &value.monitorGainDb);
    readDouble(QStringLiteral("singleSuppressionCoefficient"),
               defaults.singleSuppressionCoefficient,
               &value.singleSuppressionCoefficient);
    readDouble(QStringLiteral("dualB1SuppressionCoefficient"),
               defaults.dualB1SuppressionCoefficient,
               &value.dualB1SuppressionCoefficient);
    readDouble(QStringLiteral("dualB2SuppressionCoefficient"),
               defaults.dualB2SuppressionCoefficient,
               &value.dualB2SuppressionCoefficient);
    readDouble(QStringLiteral("bandPassLowCutoffHz"),
               defaults.bandPass.lowCutoffHz,
               &value.bandPass.lowCutoffHz);
    readDouble(QStringLiteral("bandPassHighCutoffHz"),
               defaults.bandPass.highCutoffHz,
               &value.bandPass.highCutoffHz);
    int logMmseFftSize = defaults.logMmse.fftSize;
    if (!readIntegerSetting(
            m_settings->value(
                group + QStringLiteral("logMmseFftSize"),
                defaults.logMmse.fftSize),
            &logMmseFftSize)) {
        logMmseFftSize = defaults.logMmse.fftSize;
    }
    value.logMmse.fftSize = logMmseFftSize;
    readDouble(QStringLiteral("logMmseOverlapRatio"),
               defaults.logMmse.overlapRatio,
               &value.logMmse.overlapRatio);
    readDouble(QStringLiteral("logMmsePriorSnrSmoothing"),
               defaults.logMmse.priorSnrSmoothing,
               &value.logMmse.priorSnrSmoothing);
    readDouble(QStringLiteral("logMmseNoisePowerSmoothing"),
               defaults.logMmse.noisePowerSmoothing,
               &value.logMmse.noisePowerSmoothing);
    readDouble(QStringLiteral("logMmseMinimumGainDb"),
               defaults.logMmse.minimumGainDb,
               &value.logMmse.minimumGainDb);
    readDouble(QStringLiteral("agcTargetLevelDbfs"),
               defaults.automaticGainControl.targetLevelDbfs,
               &value.automaticGainControl.targetLevelDbfs);
    readDouble(QStringLiteral("agcMaximumGainDb"),
               defaults.automaticGainControl.maximumGainDb,
               &value.automaticGainControl.maximumGainDb);
    readDouble(QStringLiteral("agcMaximumAttenuationDb"),
               defaults.automaticGainControl.maximumAttenuationDb,
               &value.automaticGainControl.maximumAttenuationDb);
    readDouble(QStringLiteral("agcAttackMilliseconds"),
               defaults.automaticGainControl.attackMilliseconds,
               &value.automaticGainControl.attackMilliseconds);
    readDouble(QStringLiteral("agcReleaseMilliseconds"),
               defaults.automaticGainControl.releaseMilliseconds,
               &value.automaticGainControl.releaseMilliseconds);
    readDouble(QStringLiteral("agcLimiterLevelDbfs"),
               defaults.automaticGainControl.limiterLevelDbfs,
               &value.automaticGainControl.limiterLevelDbfs);

    readDouble(QStringLiteral("mappingAzimuthMinimumDegrees"),
               defaults.mapping.azimuthMinimumDegrees,
               &value.mapping.azimuthMinimumDegrees);
    readDouble(QStringLiteral("mappingAzimuthMaximumDegrees"),
               defaults.mapping.azimuthMaximumDegrees,
               &value.mapping.azimuthMaximumDegrees);
    readDouble(QStringLiteral("mappingElevationMinimumDegrees"),
               defaults.mapping.elevationMinimumDegrees,
               &value.mapping.elevationMinimumDegrees);
    readDouble(QStringLiteral("mappingElevationMaximumDegrees"),
               defaults.mapping.elevationMaximumDegrees,
               &value.mapping.elevationMaximumDegrees);
    if (!value.mapping.isValid()) {
        value.mapping = defaults.mapping;
    }

    auto readTarget = [this, &group, &value](
                          const QString &prefix,
                          const ContinuousDirectionTarget &fallback) {
        ContinuousDirectionTarget target = fallback;
        const QString azimuthDegreesKey =
            group + prefix + QStringLiteral("AzimuthDegrees");
        const QString elevationDegreesKey =
            group + prefix + QStringLiteral("ElevationDegrees");
        if (m_settings->contains(azimuthDegreesKey)
            && m_settings->contains(elevationDegreesKey)) {
            if (!readFiniteNumberSetting(
                    m_settings->value(azimuthDegreesKey),
                    &target.azimuthDegrees)) {
                target.azimuthDegrees = fallback.azimuthDegrees;
            }
            if (!readFiniteNumberSetting(
                    m_settings->value(elevationDegreesKey),
                    &target.elevationDegrees)) {
                target.elevationDegrees = fallback.elevationDegrees;
            }
        } else {
            int azimuth = DirectionControlMapper::azimuthCode(
                fallback.azimuthDegrees, value.mapping);
            int elevation = DirectionControlMapper::elevationCode(
                fallback.elevationDegrees, value.mapping);
            if (!readIntegerSetting(
                    m_settings->value(
                        group + prefix + QStringLiteral("AzimuthCode"),
                        azimuth),
                    &azimuth)
                || azimuth < -4 || azimuth > 4) {
                azimuth = DirectionControlMapper::azimuthCode(
                    fallback.azimuthDegrees, value.mapping);
            }
            if (!readIntegerSetting(
                    m_settings->value(
                        group + prefix + QStringLiteral("ElevationCode"),
                        elevation),
                    &elevation)
                || elevation < -3 || elevation > 3) {
                elevation = DirectionControlMapper::elevationCode(
                    fallback.elevationDegrees, value.mapping);
            }
            target.azimuthDegrees =
                DirectionControlMapper::azimuthDegrees(
                    static_cast<qint16>(azimuth), value.mapping);
            target.elevationDegrees =
                DirectionControlMapper::elevationDegrees(
                    static_cast<qint16>(elevation), value.mapping);
        }
        return DirectionControlMapper::clampDirection(
            target, value.mapping);
    };
    value.enhancedA =
        readTarget(QStringLiteral("enhancedA"), defaults.enhancedA);
    value.suppressionB1 =
        readTarget(QStringLiteral("suppressionB1"), defaults.suppressionB1);
    value.suppressionB2 =
        readTarget(QStringLiteral("suppressionB2"), defaults.suppressionB2);

    readDouble(QStringLiteral("cameraCenterAzimuthDegrees"),
               value.camera.centerAzimuthDegrees,
               &value.camera.centerAzimuthDegrees);
    readDouble(QStringLiteral("cameraCenterElevationDegrees"),
               value.camera.centerElevationDegrees,
               &value.camera.centerElevationDegrees);
    readDouble(QStringLiteral("cameraHorizontalFovDegrees"),
               value.camera.horizontalFovDegrees,
               &value.camera.horizontalFovDegrees);
    readDouble(QStringLiteral("cameraVerticalFovDegrees"),
               value.camera.verticalFovDegrees,
               &value.camera.verticalFovDegrees);
    readDouble(QStringLiteral("cameraRollDegrees"),
               value.camera.rollDegrees,
               &value.camera.rollDegrees);
    return sanitizeNoiseReductionSettings(value);
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

void AppSettings::saveAppearance(const AppearanceSettings &settingsValue)
{
    const QString theme = settingsValue.theme.trimmed().toLower();
    m_settings->setValue(QStringLiteral("appearance/theme"),
                         theme == QStringLiteral("dark")
                             ? QStringLiteral("dark")
                             : QStringLiteral("light"));
    m_settings->sync();
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
    m_settings->setValue(
        QStringLiteral(
            "audioAnalysis/neuralPeakEnhancementEnabled"),
        value.neuralPeakEnhancementEnabled);
    m_settings->setValue(
        QStringLiteral(
            "audioAnalysis/neuralPeakEnhancementStrength"),
        value.neuralPeakEnhancementStrength);
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
    m_settings->setValue(group + QStringLiteral("spatialResolutionMode"),
                         spatialResolutionModeKey(value.spatialResolutionMode));
    m_settings->setValue(group + QStringLiteral("heatmapOpacity"), value.heatmapOpacity);
    m_settings->setValue(group + QStringLiteral("heatmapFocusExponent"),
                         value.heatmapFocusExponent);
    m_settings->setValue(group + QStringLiteral("heatmapGridColumns"),
                         value.heatmapGridColumns);
    m_settings->setValue(group + QStringLiteral("heatmapGridRows"),
                         value.heatmapGridRows);
    m_settings->setValue(group + QStringLiteral("windowSamples"), value.windowSamples);
    m_settings->setValue(group + QStringLiteral("updateIntervalMs"), value.updateIntervalMs);
    m_settings->setValue(group + QStringLiteral("soundSpeedMetersPerSecond"),
                         value.soundSpeedMetersPerSecond);
    m_settings->setValue(group + QStringLiteral("smoothingFactor"), value.smoothingFactor);
    m_settings->setValue(group + QStringLiteral("confidenceThreshold"),
                         value.confidenceThreshold);
    m_settings->setValue(
        group + QStringLiteral("trustedSourceThreshold"),
        value.confidenceThreshold);
    m_settings->setValue(
        group
            + QStringLiteral(
                "neuralPeakEnhancementEnabled"),
        value.neuralPeakEnhancementEnabled);
    m_settings->setValue(
        group
            + QStringLiteral(
                "neuralPeakEnhancementStrength"),
        value.neuralPeakEnhancementStrength);
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
    m_settings->setValue(group + QStringLiteral("showCalibrationOverlay"),
                         value.showCalibrationOverlay);
    m_settings->setValue(group + QStringLiteral("calibrationReferenceVideoWidth"),
                         value.calibrationReferenceVideoSize.width());
    m_settings->setValue(group + QStringLiteral("calibrationReferenceVideoHeight"),
                         value.calibrationReferenceVideoSize.height());
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

void AppSettings::saveNoiseReduction(
    const NoiseReductionSettings &settingsValue)
{
    const NoiseReductionSettings value =
        sanitizeNoiseReductionSettings(settingsValue);
    const QString group = QStringLiteral("noiseReduction/");
    m_settings->setValue(group + QStringLiteral("enabled"), value.enabled);
    m_settings->setValue(group + QStringLiteral("algorithm"),
                         noiseAlgorithmKey(value.algorithm));
    m_settings->setValue(group + QStringLiteral("mode"),
                         noiseModeKey(value.mode));
    m_settings->setValue(group + QStringLiteral("singleSuppressionCoefficient"),
                         value.singleSuppressionCoefficient);
    m_settings->setValue(group + QStringLiteral("dualB1SuppressionCoefficient"),
                         value.dualB1SuppressionCoefficient);
    m_settings->setValue(group + QStringLiteral("dualB2SuppressionCoefficient"),
                         value.dualB2SuppressionCoefficient);
    m_settings->setValue(group + QStringLiteral("monitorEnabled"),
                         value.monitorEnabled);
    m_settings->setValue(group + QStringLiteral("monitorBufferMilliseconds"),
                         value.monitorBufferMilliseconds);
    m_settings->setValue(group + QStringLiteral("monitorSource"),
                         noiseSourceKey(value.monitorSource));
    m_settings->setValue(group + QStringLiteral("monitorGainDb"),
                         value.monitorGainDb);
    m_settings->setValue(group + QStringLiteral("firstPlotSource"),
                         noiseSourceKey(value.firstPlotSource));
    m_settings->setValue(group + QStringLiteral("secondPlotSource"),
                         noiseSourceKey(value.secondPlotSource));
    m_settings->setValue(group + QStringLiteral("showMarkers"),
                         value.showMarkers);
    m_settings->setValue(group + QStringLiteral("bandPassEnabled"),
                         value.bandPass.enabled);
    m_settings->setValue(group + QStringLiteral("bandPassPreset"),
                         bandPassPresetKey(value.bandPass.preset));
    m_settings->setValue(group + QStringLiteral("bandPassLowCutoffHz"),
                         value.bandPass.lowCutoffHz);
    m_settings->setValue(group + QStringLiteral("bandPassHighCutoffHz"),
                         value.bandPass.highCutoffHz);
    QStringList orderKeys;
    orderKeys.reserve(value.postProcessingOrder.size());
    for (NoisePostProcessingNode node : value.postProcessingOrder) {
        orderKeys.append(postProcessingNodeKey(node));
    }
    m_settings->setValue(group + QStringLiteral("postProcessingOrder"),
                         orderKeys);
    m_settings->setValue(group + QStringLiteral("controlTabIndex"),
                         value.controlTabIndex);
    m_settings->setValue(group + QStringLiteral("selectedDataFlowNode"),
                         value.selectedDataFlowNode);
    m_settings->setValue(group + QStringLiteral("logMmseEnabled"),
                         value.logMmse.enabled);
    m_settings->setValue(group + QStringLiteral("logMmseFftSize"),
                         value.logMmse.fftSize);
    m_settings->setValue(group + QStringLiteral("logMmseOverlapRatio"),
                         value.logMmse.overlapRatio);
    m_settings->setValue(group + QStringLiteral("logMmsePriorSnrSmoothing"),
                         value.logMmse.priorSnrSmoothing);
    m_settings->setValue(group + QStringLiteral("logMmseNoisePowerSmoothing"),
                         value.logMmse.noisePowerSmoothing);
    m_settings->setValue(group + QStringLiteral("logMmseMinimumGainDb"),
                         value.logMmse.minimumGainDb);
    m_settings->setValue(group + QStringLiteral("agcEnabled"),
                         value.automaticGainControl.enabled);
    m_settings->setValue(group + QStringLiteral("agcTargetLevelDbfs"),
                         value.automaticGainControl.targetLevelDbfs);
    m_settings->setValue(group + QStringLiteral("agcMaximumGainDb"),
                         value.automaticGainControl.maximumGainDb);
    m_settings->setValue(
        group + QStringLiteral("agcMaximumAttenuationDb"),
        value.automaticGainControl.maximumAttenuationDb);
    m_settings->setValue(group + QStringLiteral("agcAttackMilliseconds"),
                         value.automaticGainControl.attackMilliseconds);
    m_settings->setValue(group + QStringLiteral("agcReleaseMilliseconds"),
                         value.automaticGainControl.releaseMilliseconds);
    m_settings->setValue(group + QStringLiteral("agcLimiterLevelDbfs"),
                         value.automaticGainControl.limiterLevelDbfs);
    auto saveTarget = [this, &group](
                          const QString &prefix,
                          const ContinuousDirectionTarget &target) {
        m_settings->setValue(
            group + prefix + QStringLiteral("AzimuthDegrees"),
            target.azimuthDegrees);
        m_settings->setValue(
            group + prefix + QStringLiteral("ElevationDegrees"),
            target.elevationDegrees);
    };
    saveTarget(QStringLiteral("enhancedA"), value.enhancedA);
    saveTarget(QStringLiteral("suppressionB1"), value.suppressionB1);
    saveTarget(QStringLiteral("suppressionB2"), value.suppressionB2);
    m_settings->setValue(
        group + QStringLiteral("mappingAzimuthMinimumDegrees"),
        value.mapping.azimuthMinimumDegrees);
    m_settings->setValue(
        group + QStringLiteral("mappingAzimuthMaximumDegrees"),
        value.mapping.azimuthMaximumDegrees);
    m_settings->setValue(
        group + QStringLiteral("mappingElevationMinimumDegrees"),
        value.mapping.elevationMinimumDegrees);
    m_settings->setValue(
        group + QStringLiteral("mappingElevationMaximumDegrees"),
        value.mapping.elevationMaximumDegrees);
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
