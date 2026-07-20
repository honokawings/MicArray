#include "ui/trendchartwidget.h"

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include <QPainter>
#include <QVBoxLayout>

#include <algorithm>

namespace MicArray {

TrendChartWidget::TrendChartWidget(QWidget *parent)
    : QWidget(parent)
{
    m_chart = new QChart;
    m_chart->setTitle(QStringLiteral("最近 60 秒接收趋势（Mb/s / FPS）"));
    m_chart->setTheme(QChart::ChartThemeDark);
    m_chart->legend()->setAlignment(Qt::AlignBottom);

    m_throughputSeries = new QLineSeries(m_chart);
    m_throughputSeries->setName(QStringLiteral("接收 Mb/s"));
    m_frameRateSeries = new QLineSeries(m_chart);
    m_frameRateSeries->setName(QStringLiteral("帧率 FPS"));
    m_chart->addSeries(m_throughputSeries);
    m_chart->addSeries(m_frameRateSeries);

    m_timeAxis = new QValueAxis(m_chart);
    m_timeAxis->setLabelFormat(QStringLiteral("%.0f"));
    m_timeAxis->setTitleText(QStringLiteral("秒"));
    m_timeAxis->setRange(0, MaximumSamples - 1);
    m_valueAxis = new QValueAxis(m_chart);
    m_valueAxis->setLabelFormat(QStringLiteral("%.1f"));
    m_valueAxis->setRange(0, 10);
    m_chart->addAxis(m_timeAxis, Qt::AlignBottom);
    m_chart->addAxis(m_valueAxis, Qt::AlignLeft);
    m_throughputSeries->attachAxis(m_timeAxis);
    m_throughputSeries->attachAxis(m_valueAxis);
    m_frameRateSeries->attachAxis(m_timeAxis);
    m_frameRateSeries->attachAxis(m_valueAxis);

    auto *view = new QChartView(m_chart, this);
    view->setRenderHint(QPainter::Antialiasing);
    view->setMinimumHeight(190);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view);
}

void TrendChartWidget::appendSample(double receiveMegabitsPerSecond, double framesPerSecond)
{
    m_throughputSeries->append(m_sampleIndex, std::max(0.0, receiveMegabitsPerSecond));
    m_frameRateSeries->append(m_sampleIndex, std::max(0.0, framesPerSecond));
    ++m_sampleIndex;
    if (m_throughputSeries->count() > MaximumSamples) {
        const int removeCount = m_throughputSeries->count() - MaximumSamples;
        m_throughputSeries->removePoints(0, removeCount);
        m_frameRateSeries->removePoints(0, removeCount);
    }
    const qreal first = std::max<qreal>(0, m_sampleIndex - MaximumSamples);
    m_timeAxis->setRange(first, std::max<qreal>(MaximumSamples - 1, m_sampleIndex - 1));
    const double maximum = std::max({10.0, receiveMegabitsPerSecond, framesPerSecond});
    m_valueAxis->setMax(std::max(m_valueAxis->max() * 0.95, maximum * 1.15));
}

void TrendChartWidget::clear()
{
    m_throughputSeries->clear();
    m_frameRateSeries->clear();
    m_sampleIndex = 0;
    m_timeAxis->setRange(0, MaximumSamples - 1);
    m_valueAxis->setRange(0, 10);
}

int TrendChartWidget::sampleCount() const
{
    return m_throughputSeries->count();
}

} // namespace MicArray
