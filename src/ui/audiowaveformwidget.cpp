#include "ui/audiowaveformwidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QSizePolicy>
#include <QtEndian>

#include <algorithm>
#include <cstdlib>

namespace MicArray {

AudioWaveformWidget::AudioWaveformWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(110);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void AudioWaveformWidget::appendPcm16(const QByteArray &pcm16)
{
    const qsizetype sampleCount = pcm16.size() / 2;
    m_samples.reserve(std::min<int>(m_maxSamples, m_samples.size() + static_cast<int>(sampleCount)));
    for (qsizetype i = 0; i < sampleCount; ++i) {
        const auto *ptr = reinterpret_cast<const uchar *>(pcm16.constData() + i * 2);
        m_samples.append(qFromLittleEndian<qint16>(ptr));
    }

    if (m_samples.size() > m_maxSamples) {
        m_samples.erase(m_samples.begin(), m_samples.begin() + (m_samples.size() - m_maxSamples));
    }
    update();
}

void AudioWaveformWidget::clear()
{
    m_samples.clear();
    update();
}

void AudioWaveformWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(16, 20, 24));

    const QRectF plot = rect().adjusted(10, 8, -10, -8);
    painter.setPen(QPen(QColor(55, 65, 75), 1));
    painter.drawRect(plot);

    const double centerY = plot.center().y();
    painter.setPen(QPen(QColor(80, 95, 110), 1));
    painter.drawLine(QPointF(plot.left(), centerY), QPointF(plot.right(), centerY));

    painter.setPen(QColor(170, 185, 200));
    painter.drawText(plot.adjusted(8, 4, -8, -4), Qt::AlignLeft | Qt::AlignTop, QStringLiteral("当前播放通道波形"));

    if (m_samples.size() < 2) {
        painter.setPen(QColor(130, 145, 160));
        painter.drawText(plot, Qt::AlignCenter, QStringLiteral("等待音频数据"));
        return;
    }

    int peak = 1;
    for (qint16 sample : m_samples) {
        peak = std::max(peak, std::abs(static_cast<int>(sample)));
    }
    peak = std::max(peak, 512);

    painter.setPen(QColor(120, 135, 150));
    painter.drawText(plot.adjusted(8, 22, -8, -4),
                     Qt::AlignLeft | Qt::AlignTop,
                     QStringLiteral("显示峰值: %1").arg(peak));

    QPainterPath path;
    const int visibleSamples = m_samples.size();
    for (int i = 0; i < visibleSamples; ++i) {
        const double x = plot.left() + (static_cast<double>(i) / static_cast<double>(visibleSamples - 1)) * plot.width();
        const double normalized = std::clamp(static_cast<double>(m_samples.at(i)) / static_cast<double>(peak), -1.0, 1.0);
        const double y = centerY - normalized * (plot.height() * 0.42);
        if (i == 0) {
            path.moveTo(x, y);
        } else {
            path.lineTo(x, y);
        }
    }

    painter.setPen(QPen(QColor(57, 197, 187), 2));
    painter.drawPath(path);
}

} // namespace MicArray
