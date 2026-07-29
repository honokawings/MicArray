#include "media/noisereductionframedecoder.h"

#include "media/audioframedecoder.h"

#include <QtEndian>

#include <algorithm>

namespace MicArray {

NoiseReductionInputFrame NoiseReductionFrameDecoder::decode(
    const QByteArray &payload) const
{
    NoiseReductionInputFrame frame;
    if (payload.size() != AudioFrameDecoder::FramePayloadBytes) {
        frame.error =
            QStringLiteral("Expected 38400-byte raw noise-reduction payload, got %1 bytes")
                .arg(payload.size());
        return frame;
    }

    for (QVector<float> &channel : frame.channels) {
        channel.reserve(AudioFrameDecoder::SamplesPerFrame);
    }
    for (int samplePoint = 0;
         samplePoint < AudioFrameDecoder::SamplesPerFrame;
         ++samplePoint) {
        const int sampleBase =
            samplePoint * AudioFrameDecoder::BytesPerSamplePoint;
        for (int channel = 0; channel < 4; ++channel) {
            const int offset =
                sampleBase + channel * AudioFrameDecoder::BytesPerChannel;
            const qint32 raw = qFromLittleEndian<qint32>(
                reinterpret_cast<const uchar *>(payload.constData() + offset));
            const qint32 pcm = raw >> 8;
            frame.channels[static_cast<size_t>(channel)].append(
                std::clamp(static_cast<float>(pcm) / 32768.0f, -1.0f, 1.0f));
        }
    }
    frame.valid = true;
    return frame;
}

} // namespace MicArray
