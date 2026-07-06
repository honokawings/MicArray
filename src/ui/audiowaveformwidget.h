#pragma once

#include <QByteArray>
#include <QVector>
#include <QWidget>

namespace MicArray {

class AudioWaveformWidget : public QWidget
{
    Q_OBJECT

public:
    explicit AudioWaveformWidget(QWidget *parent = nullptr);

    void appendPcm16(const QByteArray &pcm16);
    void clear();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVector<qint16> m_samples;
    int m_maxSamples = 4096;
};

} // namespace MicArray
