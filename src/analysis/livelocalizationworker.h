#pragma once

#include "analysis/ispatialresponseprovider.h"
#include "analysis/soundsourcelocatorregistry.h"
#include "app/appsettings.h"

#include <QObject>

#include <memory>

namespace MicArray {

class LiveLocalizationWorker : public QObject
{
    Q_OBJECT

public:
    explicit LiveLocalizationWorker(QObject *parent = nullptr);

public slots:
    void configure(const LiveLocalizationSettings &settings);
    void setActive(bool active);
    void setSpatialGrid(const SpatialResponseGrid &grid, quint64 generation);
    void pushAdjustedFrame(const QByteArray &payload,
                           quint32 timestampSeconds,
                           quint32 timestampMicros);
    void captureStableDirection(quint64 requestId);
    void shutdown();

signals:
    void audioFrameConsumed();
    void localizationReady(const LiveLocalizationResult &result);
    void spatialResponseReady(const LiveSpatialResponse &response);
    void calibrationSampleReady(quint64 requestId,
                                bool valid,
                                const VideoCalibrationSample &sample,
                                const QString &message);
    void statusChanged(const QString &status);
    void spatialResponseSupported(bool supported);
    void shutdownFinished();

private:
    struct TimedDirection {
        qint64 timestampMicroseconds = 0;
        SoundSourceDirection direction;
    };

    void clearStreamState();
    bool rebuildLocator();
    MultiChannelAudioWindow latestWindow() const;
    void analyzeLatest(qint64 timestampMicroseconds);

    SoundSourceLocatorRegistry m_registry;
    std::unique_ptr<ISoundSourceLocator> m_locator;
    ISpatialResponseProvider *m_spatialProvider = nullptr;
    LiveLocalizationSettings m_settings;
    SpatialResponseGrid m_grid;
    QVector<QVector<float>> m_channels;
    QVector<TimedDirection> m_directionHistory;
    QVector<float> m_smoothedSpatialValues;
    quint64 m_gridGeneration = 0;
    qint64 m_totalSamples = 0;
    qint64 m_lastAnalysisTimestamp = -1;
    qint64 m_latestFrameTimestamp = -1;
    bool m_active = false;
};

} // namespace MicArray
