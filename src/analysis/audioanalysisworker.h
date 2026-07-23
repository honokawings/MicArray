#pragma once

#include "analysis/soundsourcelocatorregistry.h"
#include "analysis/wavefilereader.h"
#include "app/appsettings.h"

#include <QAudioSink>
#include <QObject>
#include <QTimer>

#include <memory>

namespace MicArray {

enum class AnalysisPlaybackState {
    Closed,
    Loaded,
    Playing,
    Paused,
};

class AudioAnalysisWorker : public QObject
{
    Q_OBJECT

public:
    explicit AudioAnalysisWorker(QObject *parent = nullptr);
    ~AudioAnalysisWorker() override;

public slots:
    void loadFile(const QString &path, quint64 requestId);
    void play();
    void pause();
    void stop();
    void seek(qint64 samplePosition);
    void setOutputChannel(int channel);
    void setVolume(double volume);
    void setLocator(const QString &locatorId);
    void configureAnalysis(const AudioAnalysisSettings &settings);
    void analyzeAt(qint64 centerSample, quint64 requestId);
    void shutdown();

signals:
    void fileLoaded(const WaveFileInfo &info, quint64 requestId);
    void stateChanged(AnalysisPlaybackState state);
    void positionChanged(qint64 samplePosition);
    void waveformReady(const QByteArray &pcm16);
    void directionsReady(quint64 requestId,
                         const QVector<SoundSourceDirection> &directions,
                         qint64 elapsedMilliseconds);
    void analysisStatusChanged(const QString &status);
    void errorOccurred(const QString &message);
    void shutdownFinished();

private:
    void ensureTimers();
    bool configureLocator();
    bool startAudioAt(qint64 samplePosition);
    void stopAudioOutput();
    void feedAudio();
    void updatePlaybackPosition();
    void schedulePlaybackAnalysis();
    bool onOwningThread() const;

    SoundSourceLocatorRegistry m_registry;
    std::unique_ptr<ISoundSourceLocator> m_locator;
    std::unique_ptr<WaveFileReader> m_reader;
    std::unique_ptr<QAudioSink> m_sink;
    QIODevice *m_audioOutput = nullptr;
    std::unique_ptr<QTimer> m_feedTimer;
    std::unique_ptr<QTimer> m_analysisTimer;
    AudioAnalysisSettings m_settings;
    AnalysisPlaybackState m_state = AnalysisPlaybackState::Closed;
    qint64 m_currentSample = 0;
    qint64 m_playStartSample = 0;
    qint64 m_nextFeedSample = 0;
    quint64 m_latestRequestId = 0;
    quint64 m_internalRequestId = 0;
    quint64 m_generation = 0;
};

} // namespace MicArray

Q_DECLARE_METATYPE(MicArray::AnalysisPlaybackState)
