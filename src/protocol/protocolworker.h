#pragma once

#include <QObject>

#include "protocol/protocolcodec.h"

namespace MicArray {

class ProtocolWorker : public QObject
{
    Q_OBJECT

public:
    explicit ProtocolWorker(QObject *parent = nullptr);

public slots:
    void ingestBytes(const QByteArray &data);
    void clear();

signals:
    void frameParsed(const HostFrame &frame);
    void audioFrameParsed(const HostFrame &frame);
    void videoFrameParsed(const HostFrame &frame);
    void parseError(const ParseError &error);
    void warningMessage(const QString &module, const QString &message);
    void statisticsChanged(quint64 frames, quint64 errors);
    void pipelineMetricsChanged(quint64 bytesAppended, quint64 bytesParsed, quint64 bytesDiscarded, qint64 bufferedBytes);
    void bytesProcessed(qint64 bytes);

private:
    ProtocolCodec m_codec;
    quint64 m_frames = 0;
    quint64 m_errors = 0;
    quint64 m_bytesAppended = 0;
    quint64 m_bytesParsed = 0;
    quint64 m_bytesDiscarded = 0;
};

} // namespace MicArray
