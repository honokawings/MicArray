#include "media/noisereductiontypes.h"

namespace MicArray {

QString noiseSignalSourceName(NoiseSignalSource source)
{
    switch (source) {
    case NoiseSignalSource::FirstElement:
        return QStringLiteral("CH0 第一阵元");
    case NoiseSignalSource::EnhancedA:
        return QStringLiteral("CH1 增强 A");
    case NoiseSignalSource::SuppressionB1:
        return QStringLiteral("CH2 抑制 B1");
    case NoiseSignalSource::SuppressionB2:
        return QStringLiteral("CH3 抑制 B2");
    case NoiseSignalSource::Denoised:
        return QStringLiteral("降噪输出");
    }
    return QStringLiteral("未知音频源");
}

} // namespace MicArray
