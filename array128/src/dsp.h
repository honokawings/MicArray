#pragma once

#include "geometry.h"
#include "media/wienernoisereducer.h"
#include "media/bandpassfilter.h"
#include "media/logmmseprocessor.h"
#include "media/automaticgaincontrol.h"

#include <array>

namespace MicArray::Array128 {

struct ProcessingSettings
{
    Geometry geometry;
    bool localization = false;
    double confidenceThreshold = 0.35;
    double soundSpeed = 343.0;
    int gridColumns = 72;
    int gridRows = 45;
    CameraViewSettings camera;
    bool denoise = false;
    std::array<ScanDirection, 3> directions {{{0, 0}, {-30, 0}, {30, 0}}};
    bool dual = true;
    bool adaptive = false;
    double coefficientB1 = 5;
    double coefficientB2 = 5;
    bool bandpass = false;
    double lowHz = 100;
    double highHz = 8000;
    bool logmmse = false;
    bool agc = false;
    QVector<int> postOrder {0, 1, 2};
    bool monitor = false;
    int monitorSource = 0; // 0 raw; 1..3 A/B1/B2; 4 Wiener; 5 bandpass; 6 LogMMSE; 7 AGC
    int monitorChannel = 0;
    int bufferMs = 250;
    double monitorGainDb = 0;
    quint64 generation = 0;
    bool operator==(const ProcessingSettings &) const = default;
};

bool onlyDirectionChange(const ProcessingSettings &before, const ProcessingSettings &after);

struct LocalizationResult
{
    quint64 generation = 0;
    quint64 run = 0;
    quint64 lastFrame = 0;
    QVector<float> heat;
    int columns = 0;
    int rows = 0;
    ScanDirection direction;
    double confidence = 0;
    bool trusted = false;
    int validChannels = 0;
    int validPairs = 0;
    double elapsedMs = 0;
    QString reason;
};

// Independent 128-channel SRP-PHAT: no 48-channel neural weights or geometry.
LocalizationResult localize(const QVector<QVector<float>> &channels,
                            const ProcessingSettings &settings);

class Beamformer
{
public:
    void configure(const Geometry &geometry, const std::array<ScanDirection, 3> &directions, double speed);
    void reset();
    std::array<QVector<float>, 3> process(const QVector<QVector<float>> &channels);
private:
    struct Tap { int integer = 0; std::array<double, 4> weights {}; };
    std::array<QVector<Tap>, 3> m_taps;
    QVector<QVector<float>> m_ring;
    int m_ringSize = 0;
    int m_position = 0;
    Geometry m_geometry;
    double m_speed = 0;
};

class Denoiser
{
public:
    void configure(const ProcessingSettings &settings);
    void reset();
    // A/B1/B2, suppression, bandpass, LogMMSE, AGC; each stage retains its identity after reorder.
    std::array<QVector<float>, 7> process(const QVector<QVector<float>> &channels);
private:
    ProcessingSettings m_settings;
    Beamformer m_beamformer;
    WienerNoiseReducer m_wiener;
    BandPassFilter m_bandpass;
    LogMmseProcessor m_logmmse;
    AutomaticGainControl m_agc;
    bool m_configured = false;
};

} // namespace MicArray::Array128
