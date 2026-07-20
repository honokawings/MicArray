#include "protocol/protocolworker.h"

namespace MicArray {

ProtocolWorker::ProtocolWorker(QObject *parent)
    : QObject(parent)
{
    m_errorTimer.start();
}

void ProtocolWorker::ingestBytes(const QByteArray &data)
{
    const ParseResult result = m_codec.parseIncoming(QByteArrayView(data));
    m_bytesAppended += static_cast<quint64>(result.bytesAppended);
    m_bytesParsed += static_cast<quint64>(result.bytesParsed);
    m_bytesDiscarded += static_cast<quint64>(result.bytesDiscarded);

    for (const ParseError &error : result.errors) {
        ++m_errors;
        emit parseError(error);
        const AlertDecision decision = m_warningCoalescer.record(QString::number(static_cast<int>(error.code)), m_errorTimer.elapsed());
        if (decision.shouldEmit) {
            const QString suffix = decision.suppressedCount > 0
                ? QStringLiteral("; %1 repeated warnings suppressed").arg(decision.suppressedCount)
                : QString();
            emit warningMessage(QStringLiteral("protocol"), error.message + suffix);
        }
    }

    for (const HostFrame &frame : result.frames) {
        ++m_frames;
        if (frame.rawType == static_cast<quint16>(FrameType::Audio)) {
            emit audioFrameParsed(frame);
        } else if (frame.rawType == static_cast<quint16>(FrameType::Video)) {
            emit videoFrameParsed(frame);
        }
    }

    for (const HostFrame &frame : result.debugFrames) {
        const bool mediaFrame = frame.rawType == static_cast<quint16>(FrameType::Audio)
            || frame.rawType == static_cast<quint16>(FrameType::Video);
        if (!mediaFrame || !m_debugSampleTimer.isValid() || m_debugSampleTimer.elapsed() >= 100) {
            emit frameParsed(frame);
            m_debugSampleTimer.restart();
        }
    }

    emit statisticsChanged(m_frames, m_errors);
    emit pipelineMetricsChanged(m_bytesAppended, m_bytesParsed, m_bytesDiscarded, static_cast<qint64>(result.bufferAfter));
    emit bytesProcessed(static_cast<qint64>(data.size()));
}

void ProtocolWorker::clear()
{
    const QList<HostFrame> pendingDebugFrames = m_codec.clear();
    for (const HostFrame &frame : pendingDebugFrames) {
        emit frameParsed(frame);
    }
    m_frames = 0;
    m_errors = 0;
    m_bytesAppended = 0;
    m_bytesParsed = 0;
    m_bytesDiscarded = 0;
    m_debugSampleTimer.invalidate();
    m_warningCoalescer.clear();
    emit statisticsChanged(m_frames, m_errors);
    emit pipelineMetricsChanged(m_bytesAppended, m_bytesParsed, m_bytesDiscarded, 0);
}

} // namespace MicArray
