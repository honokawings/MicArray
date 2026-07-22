#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

#include "media/channelgainprocessor.h"

class QCheckBox;
class QSlider;
class QToolButton;

namespace MicArray {

class ArrayHeatmapWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ArrayHeatmapWidget(QWidget *parent = nullptr);

    void setAmplitudes(const QVector<double> &normalizedAmplitudes, int selectedChannel);
    void setSelectedChannel(int channel);
    void setGainConfiguration(const GainConfiguration &configuration);
    void setGainSnapshot(const GainSnapshot &snapshot);
    int selectedChannel() const { return m_selectedChannel; }
    int peakChannel() const { return m_peakChannel; }

signals:
    void channelSelected(int channel);
    void channelShiftChanged(int channel, int shift);
    void channelAutomaticChanged(int channel, bool enabled);
    void setAllAutomaticRequested(bool enabled);
    void resetRequested();
    void saveRequested();
    void loadRequested();

private:
    void refreshCells();
    void setAllAutomaticUi(bool automatic);
    void resetGainUi();
    static QColor colorForAmplitude(double value);

    QVector<QToolButton *> m_cells;
    QVector<QSlider *> m_gainSliders;
    QVector<QCheckBox *> m_automaticChecks;
    QVector<double> m_amplitudes;
    QVector<int> m_shifts;
    QVector<bool> m_automatic;
    int m_selectedChannel = 0;
    int m_peakChannel = -1;
};

} // namespace MicArray
