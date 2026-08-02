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

QString noiseReductionDataFlowStageName(NoiseReductionDataFlowStage stage)
{
    switch (stage) {
    case NoiseReductionDataFlowStage::Suppression:
        return QStringLiteral("降噪抑制");
    case NoiseReductionDataFlowStage::BandPass:
        return QStringLiteral("带通滤波");
    case NoiseReductionDataFlowStage::LogMmse:
        return QStringLiteral("LogMMSE");
    case NoiseReductionDataFlowStage::AutomaticGainControl:
        return QStringLiteral("自动增益控制");
    }
    return QStringLiteral("未知阶段");
}

QString noiseReductionDataFlowStageKey(NoiseReductionDataFlowStage stage)
{
    switch (stage) {
    case NoiseReductionDataFlowStage::Suppression:
        return QStringLiteral("suppression");
    case NoiseReductionDataFlowStage::BandPass:
        return QStringLiteral("bandpass");
    case NoiseReductionDataFlowStage::LogMmse:
        return QStringLiteral("logmmse");
    case NoiseReductionDataFlowStage::AutomaticGainControl:
        return QStringLiteral("agc");
    }
    return QStringLiteral("unknown");
}

QVector<NoisePostProcessingNode> normalizedPostProcessingOrder(
    const QVector<NoisePostProcessingNode> &order)
{
    const QVector<NoisePostProcessingNode> defaults {
        NoisePostProcessingNode::BandPass,
        NoisePostProcessingNode::LogMmse,
        NoisePostProcessingNode::AutomaticGainControl,
    };
    QVector<NoisePostProcessingNode> result;
    result.reserve(defaults.size());
    for (NoisePostProcessingNode node : order) {
        if (defaults.contains(node) && !result.contains(node)) {
            result.append(node);
        }
    }
    for (NoisePostProcessingNode node : defaults) {
        if (!result.contains(node)) {
            result.append(node);
        }
    }
    return result;
}

} // namespace MicArray
