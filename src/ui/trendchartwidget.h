#pragma once

#include <QWidget>

class QChart;
class QLineSeries;
class QValueAxis;

namespace MicArray {

class TrendChartWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TrendChartWidget(QWidget *parent = nullptr);

    void appendSample(double receiveMegabitsPerSecond, double framesPerSecond);
    void clear();
    int sampleCount() const;

private:
    static constexpr int MaximumSamples = 60;
    QChart *m_chart = nullptr;
    QLineSeries *m_throughputSeries = nullptr;
    QLineSeries *m_frameRateSeries = nullptr;
    QValueAxis *m_timeAxis = nullptr;
    QValueAxis *m_valueAxis = nullptr;
    qreal m_sampleIndex = 0;
};

} // namespace MicArray
