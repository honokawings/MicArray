#pragma once

#include <QVector>
#include <QWidget>

namespace MicArray {

class PowerSpectrumWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PowerSpectrumWidget(QWidget *parent = nullptr);

    void setSpectrum(const QString &sourceName,
                     const QVector<float> &spectrumDbfs,
                     int sampleRate);
    void clear();

    QString sourceName() const { return m_sourceName; }
    QVector<float> spectrum() const { return m_spectrumDbfs; }
    int sampleRate() const { return m_sampleRate; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_sourceName;
    QVector<float> m_spectrumDbfs;
    int m_sampleRate = 48000;
};

} // namespace MicArray
