#include "analysis/audioanalysisworker.h"

#include "analysis/arraygeometry.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QElapsedTimer>
#include <QMediaDevices>
#include <QThread>

#include <algorithm>
#include <exception>

namespace MicArray {

AudioAnalysisWorker::AudioAnalysisWorker(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<WaveFileInfo>("MicArray::WaveFileInfo");
    qRegisterMetaType<AnalysisPlaybackState>("MicArray::AnalysisPlaybackState");
    qRegisterMetaType<QVector<SoundSourceDirection>>("QVector<MicArray::SoundSourceDirection>");
    registerBuiltInSoundSourceLocators(&m_registry);
    m_locator = m_registry.create(m_settings.locatorId);
}

AudioAnalysisWorker::~AudioAnalysisWorker()
{
    stopAudioOutput();
}

void AudioAnalysisWorker::loadFile(const QString &path, quint64 requestId)
{
    Q_ASSERT(onOwningThread());
    if (requestId < m_latestRequestId) {
        return;
    }
    m_latestRequestId = requestId;
    auto candidate = std::make_unique<WaveFileReader>();
    QString error;
    if (!candidate->open(path, &error)) {
        emit errorOccurred(error);
        return;
    }

    stopAudioOutput();
    m_reader = std::move(candidate);
    m_currentSample = 0;
    m_playStartSample = 0;
    m_nextFeedSample = 0;
    ++m_generation;
    m_settings.lastWavePath = path;
    m_settings.outputChannel = std::clamp(m_settings.outputChannel,
                                          0,
                                          m_reader->info().channelCount - 1);
    m_state = AnalysisPlaybackState::Loaded;
    emit stateChanged(m_state);
    emit positionChanged(m_currentSample);
    emit fileLoaded(m_reader->info(), requestId);
    if (m_reader->info().channelCount == 48) {
        if (configureLocator()) {
            emit analysisStatusChanged(QStringLiteral("已加载 48 通道录音，可进行声源定位"));
        }
    } else {
        emit analysisStatusChanged(
            QStringLiteral("当前文件不是 48 通道，仅支持单通道播放，无法进行声源定位"));
    }
}

void AudioAnalysisWorker::play()
{
    Q_ASSERT(onOwningThread());
    if (m_reader == nullptr || !m_reader->isOpen()) {
        emit errorOccurred(QStringLiteral("请先打开 WAV 文件"));
        return;
    }
    ensureTimers();
    if (m_state == AnalysisPlaybackState::Paused && m_sink != nullptr) {
        m_sink->resume();
        m_feedTimer->start();
        m_analysisTimer->start(m_settings.updateIntervalMs);
        m_state = AnalysisPlaybackState::Playing;
        emit stateChanged(m_state);
        return;
    }
    if (m_state == AnalysisPlaybackState::Playing) {
        return;
    }
    startAudioAt(m_currentSample);
}

void AudioAnalysisWorker::pause()
{
    Q_ASSERT(onOwningThread());
    if (m_state != AnalysisPlaybackState::Playing || m_sink == nullptr) {
        return;
    }
    updatePlaybackPosition();
    m_sink->suspend();
    if (m_feedTimer != nullptr) {
        m_feedTimer->stop();
    }
    if (m_analysisTimer != nullptr) {
        m_analysisTimer->stop();
    }
    m_state = AnalysisPlaybackState::Paused;
    emit stateChanged(m_state);
}

void AudioAnalysisWorker::stop()
{
    Q_ASSERT(onOwningThread());
    if (m_reader == nullptr) {
        return;
    }
    stopAudioOutput();
    m_currentSample = 0;
    m_state = AnalysisPlaybackState::Loaded;
    emit positionChanged(m_currentSample);
    emit stateChanged(m_state);
}

void AudioAnalysisWorker::seek(qint64 samplePosition)
{
    Q_ASSERT(onOwningThread());
    if (m_reader == nullptr) {
        return;
    }
    const bool resumePlayback = m_state == AnalysisPlaybackState::Playing;
    stopAudioOutput();
    m_currentSample = std::clamp<qint64>(samplePosition, 0, m_reader->info().totalSamples);
    ++m_generation;
    emit positionChanged(m_currentSample);
    if (resumePlayback && m_currentSample < m_reader->info().totalSamples) {
        startAudioAt(m_currentSample);
    } else {
        m_state = AnalysisPlaybackState::Loaded;
        emit stateChanged(m_state);
    }
}

void AudioAnalysisWorker::setOutputChannel(int channel)
{
    Q_ASSERT(onOwningThread());
    const int channelCount = m_reader != nullptr ? m_reader->info().channelCount : 48;
    if (channel < 0 || channel >= channelCount) {
        emit errorOccurred(QStringLiteral("监听通道超出当前 WAV 通道范围"));
        return;
    }
    if (m_settings.outputChannel == channel) {
        return;
    }
    const bool resumePlayback = m_state == AnalysisPlaybackState::Playing;
    if (resumePlayback) {
        updatePlaybackPosition();
        stopAudioOutput();
    }
    m_settings.outputChannel = channel;
    if (resumePlayback) {
        startAudioAt(m_currentSample);
    }
}

void AudioAnalysisWorker::setVolume(double volume)
{
    Q_ASSERT(onOwningThread());
    m_settings.volume = std::clamp(volume, 0.0, 1.0);
    if (m_sink != nullptr) {
        m_sink->setVolume(static_cast<float>(m_settings.volume));
    }
}

void AudioAnalysisWorker::setLocator(const QString &locatorId)
{
    Q_ASSERT(onOwningThread());
    std::unique_ptr<ISoundSourceLocator> candidate = m_registry.create(locatorId);
    if (candidate == nullptr) {
        emit errorOccurred(QStringLiteral("未知声源定位算法：%1").arg(locatorId));
        return;
    }
    m_locator = std::move(candidate);
    m_settings.locatorId = locatorId;
    ++m_generation;
    if (m_reader != nullptr && m_reader->info().channelCount == 48) {
        configureLocator();
    }
}

void AudioAnalysisWorker::configureAnalysis(const AudioAnalysisSettings &settings)
{
    Q_ASSERT(onOwningThread());
    m_settings = settings;
    if (m_reader != nullptr) {
        m_settings.outputChannel = std::clamp(m_settings.outputChannel,
                                              0,
                                              m_reader->info().channelCount - 1);
    }
    std::unique_ptr<ISoundSourceLocator> candidate = m_registry.create(m_settings.locatorId);
    if (candidate == nullptr) {
        emit errorOccurred(QStringLiteral("未知声源定位算法：%1").arg(m_settings.locatorId));
        return;
    }
    m_locator = std::move(candidate);
    ++m_generation;
    if (m_analysisTimer != nullptr) {
        m_analysisTimer->setInterval(m_settings.updateIntervalMs);
    }
    if (m_sink != nullptr) {
        m_sink->setVolume(static_cast<float>(std::clamp(m_settings.volume, 0.0, 1.0)));
    }
    if (m_reader != nullptr && m_reader->info().channelCount == 48) {
        configureLocator();
    }
}

void AudioAnalysisWorker::analyzeAt(qint64 centerSample, quint64 requestId)
{
    Q_ASSERT(onOwningThread());
    if (requestId < m_latestRequestId) {
        return;
    }
    m_latestRequestId = requestId;
    if (m_reader == nullptr || !m_reader->isOpen()) {
        emit errorOccurred(QStringLiteral("请先打开 WAV 文件"));
        return;
    }

    QElapsedTimer elapsed;
    elapsed.start();
    QVector<SoundSourceDirection> directions;
    const WaveFileInfo info = m_reader->info();
    if (info.channelCount != 48) {
        emit analysisStatusChanged(
            QStringLiteral("当前文件不是 48 通道，无法进行声源定位"));
    } else if (m_locator == nullptr && !configureLocator()) {
        emit directionsReady(requestId, {}, elapsed.elapsed());
        return;
    } else {
        QString error;
        const qint64 startSample = centerSample - m_settings.windowSamples / 2;
        MultiChannelAudioWindow window;
        window.channels = m_reader->readChannels(startSample,
                                                 m_settings.windowSamples,
                                                 &error);
        window.centerSample = centerSample;
        window.sampleRate = info.sampleRate;
        if (!error.isEmpty()) {
            emit errorOccurred(error);
        } else {
            try {
                directions = m_locator->locate(window);
            } catch (const std::exception &exception) {
                emit errorOccurred(QStringLiteral("声源定位算法异常：%1")
                                       .arg(QString::fromLocal8Bit(exception.what())));
            } catch (...) {
                emit errorOccurred(QStringLiteral("声源定位算法发生未知异常"));
            }
        }
        const QByteArray waveform = m_reader->readMonoPcm16(m_settings.outputChannel,
                                                            startSample,
                                                            m_settings.windowSamples,
                                                            &error);
        if (error.isEmpty()) {
            emit waveformReady(waveform);
        }
        if (directions.isEmpty()) {
            emit analysisStatusChanged(QStringLiteral("当前时间窗未检测到可靠声源"));
        } else if (directions.first().confidence < m_settings.confidenceThreshold) {
            emit analysisStatusChanged(QStringLiteral("定位置信度低，照片标记已抑制"));
        } else {
            emit analysisStatusChanged(QStringLiteral("定位有效"));
        }
    }
    emit directionsReady(requestId, directions, elapsed.elapsed());
}

void AudioAnalysisWorker::shutdown()
{
    Q_ASSERT(onOwningThread());
    stopAudioOutput();
    if (m_reader != nullptr) {
        m_reader->close();
        m_reader.reset();
    }
    m_locator.reset();
    m_state = AnalysisPlaybackState::Closed;
    emit stateChanged(m_state);
    emit shutdownFinished();
}

void AudioAnalysisWorker::ensureTimers()
{
    if (m_feedTimer == nullptr) {
        m_feedTimer = std::make_unique<QTimer>();
        m_feedTimer->setInterval(10);
        connect(m_feedTimer.get(), &QTimer::timeout, this, &AudioAnalysisWorker::feedAudio);
    }
    if (m_analysisTimer == nullptr) {
        m_analysisTimer = std::make_unique<QTimer>();
        m_analysisTimer->setInterval(m_settings.updateIntervalMs);
        connect(m_analysisTimer.get(),
                &QTimer::timeout,
                this,
                &AudioAnalysisWorker::schedulePlaybackAnalysis);
    }
}

bool AudioAnalysisWorker::configureLocator()
{
    if (m_reader == nullptr || m_locator == nullptr) {
        return false;
    }
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    configuration.sampleRate = m_reader->info().sampleRate;
    configuration.windowSamples = m_settings.windowSamples;
    configuration.soundSpeedMetersPerSecond = m_settings.soundSpeedMetersPerSecond;
    configuration.confidenceThreshold = m_settings.confidenceThreshold;
    configuration.smoothingFactor = m_settings.smoothingFactor;
    configuration.spatialResolutionMode =
        SpatialResolutionMode::HighPrecision;
    QString error;
    if (!m_locator->configure(configuration, &error)) {
        emit errorOccurred(error);
        return false;
    }
    return true;
}

bool AudioAnalysisWorker::startAudioAt(qint64 samplePosition)
{
    ensureTimers();
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) {
        emit errorOccurred(QStringLiteral("未找到可用音频输出设备"));
        return false;
    }
    QAudioFormat format;
    format.setSampleRate(m_reader->info().sampleRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    if (!device.isFormatSupported(format)) {
        emit errorOccurred(QStringLiteral("默认声卡不支持 WAV 的单声道 PCM16 采样率"));
        return false;
    }
    stopAudioOutput();
    m_sink = std::make_unique<QAudioSink>(device, format);
    m_sink->setVolume(static_cast<float>(std::clamp(m_settings.volume, 0.0, 1.0)));
    m_audioOutput = m_sink->start();
    if (m_audioOutput == nullptr) {
        emit errorOccurred(QStringLiteral("无法启动离线 WAV 播放"));
        m_sink.reset();
        return false;
    }
    m_playStartSample = samplePosition;
    m_nextFeedSample = samplePosition;
    m_currentSample = samplePosition;
    m_state = AnalysisPlaybackState::Playing;
    m_feedTimer->start();
    m_analysisTimer->start(m_settings.updateIntervalMs);
    feedAudio();
    emit stateChanged(m_state);
    return true;
}

void AudioAnalysisWorker::stopAudioOutput()
{
    if (m_feedTimer != nullptr) {
        m_feedTimer->stop();
    }
    if (m_analysisTimer != nullptr) {
        m_analysisTimer->stop();
    }
    if (m_sink != nullptr) {
        m_sink->stop();
    }
    m_audioOutput = nullptr;
    m_sink.reset();
}

void AudioAnalysisWorker::feedAudio()
{
    if (m_state != AnalysisPlaybackState::Playing
        || m_sink == nullptr || m_audioOutput == nullptr || m_reader == nullptr) {
        return;
    }
    updatePlaybackPosition();
    const qint64 remaining = m_reader->info().totalSamples - m_nextFeedSample;
    if (remaining > 0) {
        const int availableFrames = static_cast<int>(std::min<qint64>(
            std::max<qint64>(0, m_sink->bytesFree() / 2),
            std::min<qint64>(remaining, 4096)));
        if (availableFrames > 0) {
            QString error;
            const QByteArray pcm = m_reader->readMonoPcm16(m_settings.outputChannel,
                                                           m_nextFeedSample,
                                                           availableFrames,
                                                           &error);
            if (!error.isEmpty()) {
                emit errorOccurred(error);
                stop();
                return;
            }
            const qint64 written = m_audioOutput->write(pcm);
            if (written < 0) {
                emit errorOccurred(QStringLiteral("写入声卡失败"));
                stop();
                return;
            }
            m_nextFeedSample += written / 2;
        }
    }
    if (m_nextFeedSample >= m_reader->info().totalSamples
        && m_currentSample >= m_reader->info().totalSamples - 1) {
        stopAudioOutput();
        m_currentSample = m_reader->info().totalSamples;
        m_state = AnalysisPlaybackState::Loaded;
        emit positionChanged(m_currentSample);
        emit stateChanged(m_state);
    }
}

void AudioAnalysisWorker::updatePlaybackPosition()
{
    if (m_sink == nullptr || m_reader == nullptr) {
        return;
    }
    m_currentSample = std::clamp<qint64>(
        m_playStartSample
            + m_sink->processedUSecs() * m_reader->info().sampleRate / 1'000'000,
        0,
        m_reader->info().totalSamples);
    emit positionChanged(m_currentSample);
}

void AudioAnalysisWorker::schedulePlaybackAnalysis()
{
    updatePlaybackPosition();
    m_internalRequestId = std::max(m_internalRequestId, m_latestRequestId) + 1;
    analyzeAt(m_currentSample, m_internalRequestId);
}

bool AudioAnalysisWorker::onOwningThread() const
{
    return QThread::currentThread() == thread();
}

} // namespace MicArray
