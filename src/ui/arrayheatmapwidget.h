#pragma once

#include <QVector>
#include <QWidget>

class QToolButton;

namespace MicArray {

class ArrayHeatmapWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ArrayHeatmapWidget(QWidget *parent = nullptr);

    void setAmplitudes(const QVector<double> &normalizedAmplitudes, int selectedChannel);
    void setSelectedChannel(int channel);
    int selectedChannel() const { return m_selectedChannel; }
    int peakChannel() const { return m_peakChannel; }

signals:
    void channelSelected(int channel);

private:
    void refreshCells();
    static QString colorForAmplitude(double value);

    QVector<QToolButton *> m_cells;
    QVector<double> m_amplitudes;
    int m_selectedChannel = 0;
    int m_peakChannel = -1;
};

} // namespace MicArray
