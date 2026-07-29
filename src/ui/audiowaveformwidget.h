#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QWidget>

namespace MicArray {

class AudioWaveformWidget : public QWidget
{
    Q_OBJECT

public:
    explicit AudioWaveformWidget(QWidget *parent = nullptr);

    void appendPcm16(const QByteArray &pcm16);
    void setSamples(const QVector<float> &samples);
    void setTitle(const QString &title);
    QString title() const { return m_title; }
    int sampleCount() const { return m_samples.size(); }
    void clear();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVector<qint16> m_samples;
    int m_maxSamples = 4096;
    QString m_title = QStringLiteral("当前播放通道波形");
};

} // namespace MicArray
