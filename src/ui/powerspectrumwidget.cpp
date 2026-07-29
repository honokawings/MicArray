#include "ui/powerspectrumwidget.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace MicArray {

PowerSpectrumWidget::PowerSpectrumWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void PowerSpectrumWidget::setSpectrum(
    const QString &sourceName,
    const QVector<float> &spectrumDbfs,
    int sampleRate)
{
    m_sourceName = sourceName;
    m_spectrumDbfs = spectrumDbfs;
    for (float &value : m_spectrumDbfs) {
        value = std::clamp(value, -120.0f, 0.0f);
    }
    m_sampleRate = std::clamp(sampleRate, 8000, 192000);
    update();
}

void PowerSpectrumWidget::clear()
{
    m_spectrumDbfs.clear();
    update();
}

void PowerSpectrumWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(12, 18, 26));
    const QRectF plot = rect().adjusted(48, 26, -14, -30);
    painter.setPen(QPen(QColor(55, 70, 85), 1));
    painter.drawRect(plot);
    for (int division = 1; division < 4; ++division) {
        const double y = plot.top() + plot.height() * division / 4.0;
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(QColor(125, 140, 155));
        painter.drawText(QRectF(2, y - 8, 42, 16),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(-division * 30));
        painter.setPen(QPen(QColor(55, 70, 85), 1));
    }
    for (int division = 1; division < 4; ++division) {
        const double x = plot.left() + plot.width() * division / 4.0;
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.setPen(QColor(125, 140, 155));
        const double frequency =
            (m_sampleRate * 0.5) * division / 4.0 / 1000.0;
        painter.drawText(QRectF(x - 35, plot.bottom() + 5, 70, 18),
                         Qt::AlignHCenter | Qt::AlignTop,
                         QStringLiteral("%1k").arg(frequency, 0, 'f', 1));
        painter.setPen(QPen(QColor(55, 70, 85), 1));
    }
    painter.setPen(QColor(205, 220, 235));
    painter.drawText(QRectF(plot.left(), 3, plot.width(), 20),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     m_sourceName.isEmpty()
                         ? QStringLiteral("等待功率谱")
                         : QStringLiteral("%1 · 功率谱").arg(m_sourceName));
    painter.drawText(QRectF(plot.left(), plot.bottom() + 5,
                            plot.width(), 18),
                     Qt::AlignRight | Qt::AlignTop,
                     QStringLiteral("Hz"));
    if (m_spectrumDbfs.size() < 2) {
        painter.setPen(QColor(125, 140, 155));
        painter.drawText(plot, Qt::AlignCenter, QStringLiteral("等待音频数据"));
        return;
    }

    QPainterPath curve;
    for (int index = 0; index < m_spectrumDbfs.size(); ++index) {
        const double x =
            plot.left()
            + static_cast<double>(index)
                / static_cast<double>(m_spectrumDbfs.size() - 1)
                * plot.width();
        const double normalized =
            (std::clamp(static_cast<double>(m_spectrumDbfs.at(index)),
                        -120.0,
                        0.0)
             + 120.0) / 120.0;
        const double y = plot.bottom() - normalized * plot.height();
        if (index == 0) {
            curve.moveTo(x, y);
        } else {
            curve.lineTo(x, y);
        }
    }
    painter.setPen(QPen(QColor(45, 210, 190), 1.6));
    painter.drawPath(curve);
}

} // namespace MicArray
