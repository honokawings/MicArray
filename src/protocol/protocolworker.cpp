#include "protocol/protocolworker.h"

namespace MicArray {

ProtocolWorker::ProtocolWorker(QObject *parent)
    : QObject(parent)
{
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
        emit warningMessage(QStringLiteral("protocol"), error.message);
    }

    for (const HostFrame &frame : result.frames) {
        ++m_frames;
        emit frameParsed(frame);
        if (frame.rawType == static_cast<quint16>(FrameType::Audio)) {
            emit audioFrameParsed(frame);
        } else if (frame.rawType == static_cast<quint16>(FrameType::Video)) {
            emit videoFrameParsed(frame);
        }
    }

    emit statisticsChanged(m_frames, m_errors);
    emit pipelineMetricsChanged(m_bytesAppended, m_bytesParsed, m_bytesDiscarded, static_cast<qint64>(result.bufferAfter));
    emit bytesProcessed(static_cast<qint64>(data.size()));
}

void ProtocolWorker::clear()
{
    m_codec.clear();
    m_frames = 0;
    m_errors = 0;
    m_bytesAppended = 0;
    m_bytesParsed = 0;
    m_bytesDiscarded = 0;
    emit statisticsChanged(m_frames, m_errors);
    emit pipelineMetricsChanged(m_bytesAppended, m_bytesParsed, m_bytesDiscarded, 0);
}

} // namespace MicArray
