#pragma once

#include "dsp.h"

#include <QImage>
#include <QJsonObject>
#include <deque>
#include <optional>

namespace MicArray::Array128 {

// All times use Pipeline::monotonicNowNs(). No camera/FPGA clocks are mixed.
struct PairedVideo
{
    QImage image;
    double ageMs = -1;
    double skewMs = 0;
    QString error;
};

class FusionVideoBuffer
{
public:
    void append(const QImage &image, qint64 arrivalNs);
    void clear();
    PairedVideo nearest(qint64 audioObservationNs, qint64 nowNs,
                        double maxSkewMs = 100, double maxAgeMs = 500) const;
private:
    struct Frame { QImage image; qint64 arrivalNs; };
    std::deque<Frame> m_frames;
    qsizetype m_bytes = 0;
};

struct FusionDisplayContext
{
    QImage video;
    LocalizationResult localization;
};

// Covers the 500 ms inference lifetime at 10 Hz with margin. Display previews
// are bounded separately from the unmodified inference input pixels.
class FusionDisplayBuffer
{
public:
    void store(const QImage &video, const LocalizationResult &localization);
    void clear();
    std::optional<FusionDisplayContext> find(quint64 generation, quint64 run, quint64 frame) const;
private:
    std::deque<FusionDisplayContext> m_frames;
};

// Fail closed if the live PSF does not match the model's training geometry/grid.
QString fusionDomainError(const QJsonObject &metadata, const ProcessingSettings &settings);

} // namespace MicArray::Array128
