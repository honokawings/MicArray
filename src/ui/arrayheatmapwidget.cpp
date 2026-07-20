#include "ui/arrayheatmapwidget.h"

#include <QGridLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace MicArray {

ArrayHeatmapWidget::ArrayHeatmapWidget(QWidget *parent)
    : QWidget(parent)
    , m_amplitudes(48, 0.0)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *title = new QLabel(QStringLiteral("48 通道阵列幅值（非声源方向定位）"), this);
    title->setStyleSheet(QStringLiteral("font-weight:600; color:#d9e7f5;"));
    layout->addWidget(title);

    auto *grid = new QGridLayout;
    grid->setSpacing(4);
    for (int channel = 0; channel < 48; ++channel) {
        auto *cell = new QToolButton(this);
        cell->setObjectName(QStringLiteral("channelCell_%1").arg(channel));
        cell->setMinimumSize(58, 44);
        cell->setToolButtonStyle(Qt::ToolButtonTextOnly);
        connect(cell, &QToolButton::clicked, this, [this, channel]() {
            setSelectedChannel(channel);
            emit channelSelected(channel);
        });
        grid->addWidget(cell, channel / 8, channel % 8);
        m_cells.append(cell);
    }
    layout->addLayout(grid);
    refreshCells();
}

void ArrayHeatmapWidget::setAmplitudes(const QVector<double> &normalizedAmplitudes, int selectedChannel)
{
    if (normalizedAmplitudes.size() != m_cells.size()) {
        return;
    }
    m_amplitudes = normalizedAmplitudes;
    m_selectedChannel = std::clamp(selectedChannel, 0, static_cast<int>(m_cells.size()) - 1);
    m_peakChannel = static_cast<int>(std::distance(m_amplitudes.cbegin(), std::max_element(m_amplitudes.cbegin(), m_amplitudes.cend())));
    refreshCells();
}

void ArrayHeatmapWidget::setSelectedChannel(int channel)
{
    if (channel < 0 || channel >= m_cells.size()) {
        return;
    }
    m_selectedChannel = channel;
    refreshCells();
}

void ArrayHeatmapWidget::refreshCells()
{
    for (int channel = 0; channel < m_cells.size(); ++channel) {
        QToolButton *cell = m_cells.at(channel);
        const double amplitude = std::clamp(m_amplitudes.value(channel), 0.0, 1.0);
        const bool selected = channel == m_selectedChannel;
        const bool peak = channel == m_peakChannel;
        cell->setProperty("selected", selected);
        cell->setProperty("peak", peak);
        cell->setText(QStringLiteral("CH%1\n%2%").arg(channel, 2, 10, QLatin1Char('0')).arg(amplitude * 100.0, 0, 'f', 0));
        const QString border = selected ? QStringLiteral("3px solid #43d9ff")
                                        : peak ? QStringLiteral("3px solid #ffd166")
                                               : QStringLiteral("1px solid #34495e");
        cell->setStyleSheet(QStringLiteral("QToolButton { background:%1; color:white; border:%2; border-radius:4px; font-weight:600; }")
                                .arg(colorForAmplitude(amplitude), border));
    }
}

QString ArrayHeatmapWidget::colorForAmplitude(double value)
{
    const double clamped = std::clamp(value, 0.0, 1.0);
    const int red = static_cast<int>(35 + 210 * clamped);
    const int green = static_cast<int>(70 + 90 * (1.0 - std::abs(clamped - 0.5) * 2.0));
    const int blue = static_cast<int>(170 * (1.0 - clamped));
    return QStringLiteral("rgb(%1,%2,%3)").arg(red).arg(green).arg(blue);
}

} // namespace MicArray
