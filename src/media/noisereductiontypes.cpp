#include "media/noisereductiontypes.h"

namespace MicArray {

QString plFullDenoiseProfileError(const NoiseReductionSettings &settings)
{
    if (settings.algorithm != NoiseReductionAlgorithm::FixedParameterWiener)
        return QStringLiteral("PL 全链仅支持固定参数维纳；自适应请明确选择软件或三波束模式。");
    if (!settings.bandPass.enabled || settings.bandPass.lowCutoffHz != 100.0 || settings.bandPass.highCutoffHz != 8000.0)
        return QStringLiteral("PL 全链固定演示参数要求带通开启、100–8000 Hz；当前参数未改变，请手动选择兼容配置或其他输入模式。");
    const auto &log = settings.logMmse;
    if (!log.enabled || log.fftSize != 2048 || log.overlapRatio != 0.5 || log.priorSnrSmoothing != 0.98
        || log.noisePowerSmoothing != 0.95 || log.minimumGainDb != -20.0)
        return QStringLiteral("PL 全链要求 LogMMSE 开启、2048点/重叠0.5/先验0.98/噪声0.95/最小增益−20 dB；当前设置不兼容，未自动替换。");
    const auto &agc = settings.automaticGainControl;
    if (!agc.enabled || agc.targetLevelDbfs != -24.0 || agc.maximumGainDb != 20.0 || agc.maximumAttenuationDb != -20.0
        || agc.attackMilliseconds != 20.0 || agc.releaseMilliseconds != 300.0 || agc.limiterLevelDbfs != -1.0)
        return QStringLiteral("PL 全链要求 AGC 开启、目标−24/增益20/衰减−20 dB、启动20 ms/释放300 ms、限幅−1 dB；当前设置不兼容，未自动替换。");
    if (settings.postProcessingOrder != QVector<NoisePostProcessingNode>{NoisePostProcessingNode::BandPass,
            NoisePostProcessingNode::LogMmse, NoisePostProcessingNode::AutomaticGainControl})
        return QStringLiteral("PL 全链固定顺序为带通→LogMMSE→AGC；当前顺序不兼容，未自动修改。");
    return {};
}

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
