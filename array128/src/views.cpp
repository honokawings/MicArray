#include "views.h"
#include "analysis/directionprojection.h"
#include "analysis/radix2fft.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace MicArray::Array128 {

ChannelGrid::ChannelGrid(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(480, 290);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setToolTip(QStringLiteral("这是 DATA/LR 通道索引矩阵，不代表螺旋阵元物理坐标。点击通道切换监听。"));
}
void ChannelGrid::setData(const PipelineSnapshot &snapshot, int selected)
{
    m_quality = snapshot.quality; m_selected = selected; m_hasData = !snapshot.latestHeader.isEmpty(); update();
}
void ChannelGrid::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double cw = width() / 16.0, rh = height() / 8.0;
    for (int ch = 0; ch < Channels; ++ch) {
        const auto &q = m_quality[ch];
        const double value = std::clamp((q.rmsDb + 90) / 90, 0.0, 1.0);
        QRectF box((ch % 16) * cw + 2, (ch / 16) * rh + 2, cw - 4, rh - 4);
        QColor color = QColor::fromHsvF((1 - value) * 0.60, 0.60, 0.23 + value * 0.55);
        if (!m_hasData) color = QColor("#283744");
        if (q.clipped) color = QColor("#a44238");
        p.setBrush(color);
        p.setPen(QPen(ch == m_selected ? QColor("#ffe18d") : QColor("#304456"), ch == m_selected ? 2.5 : 0.7));
        p.drawRoundedRect(box, 4, 4);
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, QStringLiteral("%1\n%2").arg(ch).arg(m_hasData ? QString::number(q.rmsDb, 'f', 0) : QStringLiteral("—")));
    }
}
void ChannelGrid::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    const int col = int(event->position().x() * 16 / width()), row = int(event->position().y() * 8 / height());
    if (col >= 0 && col < 16 && row >= 0 && row < 8) emit channelSelected(row * 16 + col);
}

WaveView::WaveView(QWidget *parent) : QWidget(parent) { setMinimumHeight(100); }
void WaveView::setSamples(const QVector<float> &samples) { m_samples = samples; update(); }
void WaveView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor("#101c29"));
    p.setPen(QColor("#283b4d"));
    for (int i = 1; i < 8; ++i) p.drawLine(i * width() / 8, 0, i * width() / 8, height());
    p.drawLine(0, height() / 2, width(), height() / 2);
    if (m_samples.size() < 2) { p.setPen(Qt::gray); p.drawText(rect(), Qt::AlignCenter, QStringLiteral("等待音频")); return; }
    QVector<float> values = m_samples;
    if (m_spectrum) {
        constexpr int n = 2048;
        QVector<std::complex<double>> spectrum(n);
        for (int i = 0; i < std::min(n, int(values.size())); ++i)
            spectrum[i] = values[i] * (0.5 - 0.5 * std::cos(6.283185307179586 * i / (n - 1)));
        Radix2Fft::transform(&spectrum, false);
        values.resize(n / 2);
        for (int i = 0; i < n / 2; ++i)
            values[i] = float(20 * std::log10(std::max(std::abs(spectrum[i]) * 4 / n, 1e-6)));
    }
    double scale = 0.01;
    for (float v : values) scale = std::max(scale, std::abs(double(v)));
    QPainterPath path;
    for (int i = 0; i < values.size(); ++i) {
        const double x = double(i) / (values.size() - 1) * width();
        const double y = m_spectrum ? height() * (1 - std::clamp((values[i] + 120) / 120.0, 0.0, 1.0))
                                    : height() * (0.5 - values[i] / scale * 0.44);
        if (!i) path.moveTo(x, y); else path.lineTo(x, y);
    }
    p.setPen(QPen(QColor("#5de0c1"), 1.2)); p.drawPath(path);
    p.setPen(QColor("#8da9be"));
    p.drawText(8, 16, m_spectrum ? QStringLiteral("幅度谱 0–24 kHz / dBFS · 硬件通带 20 Hz–16 kHz") : QStringLiteral("通道波形 · 自动显示缩放（不改变信号）"));
}

FieldView::FieldView(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(400, 260); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}
void FieldView::setImage(const QImage &image) { m_image = image; update(); }
void FieldView::setCameraView(const CameraViewSettings &camera) { m_camera = camera; update(); }
void FieldView::setTargets(const std::array<ScanDirection, 3> &targets, bool show, int active)
{
    m_targets = targets; m_showTargets = show; m_active = std::clamp(active, 0, 2); update();
}
void FieldView::setCalibrationPoints(const QVector<VideoCalibrationSample> &points) { m_calibration = points; update(); }
void FieldView::setResponse(const LocalizationResult &result, double focus)
{
    m_result = result;
    if (result.heat.size() != result.columns * result.rows || result.heat.isEmpty()) { m_heat = {}; update(); return; }
    m_heat = QImage(result.columns, result.rows, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < result.rows; ++y) for (int x = 0; x < result.columns; ++x) {
        const double value = std::pow(std::clamp(double(result.heat[y * result.columns + x]), 0.0, 1.0), focus);
        const double mapped = result.trusted ? value : value * 0.38;
        QColor color = QColor::fromHsvF((1 - mapped) * 0.67, 0.90, 1.0, 0.05 + value * (result.trusted ? 0.50 : 0.23));
        m_heat.setPixelColor(x, y, color);
    }
    update();
}
QRectF FieldView::imageRect() const
{
    QSizeF size = imageSize(); size.scale(this->size(), Qt::KeepAspectRatio);
    return QRectF((width() - size.width()) / 2, (height() - size.height()) / 2, size.width(), size.height());
}
void FieldView::paintEvent(QPaintEvent *)
{
    QPainter p(this); p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    p.fillRect(rect(), QColor("#0b1520"));
    const QRectF box = imageRect();
    if (!m_image.isNull()) p.drawImage(box, m_image);
    else {
        p.setPen(QColor("#243d50"));
        for (int i = 0; i <= 8; ++i) {
            p.drawLine(QPointF(box.left() + box.width() * i / 8, box.top()), QPointF(box.left() + box.width() * i / 8, box.bottom()));
            p.drawLine(QPointF(box.left(), box.top() + box.height() * i / 8), QPointF(box.right(), box.top() + box.height() * i / 8));
        }
        p.setPen(QColor("#9eb8cc")); p.drawText(box, Qt::AlignCenter, QStringLiteral("USB 摄像头未开启\n可使用方向网格或载入静态照片"));
    }
    if (!m_heat.isNull()) p.drawImage(box, m_heat);
    auto point = [&](const ScanDirection &d) {
        SoundSourceDirection s; s.azimuthDegrees = d.azimuthDegrees; s.elevationDegrees = d.elevationDegrees;
        const auto projection = projectDirection(s, m_camera);
        if (!projection.inView) return QPointF(-1000, -1000);
        return QPointF(box.x() + projection.normalizedPoint.x() * box.width(), box.y() + projection.normalizedPoint.y() * box.height());
    };
    if (m_result.trusted) {
        const QPointF center = point(m_result.direction);
        p.setPen(QPen(QColor("#ffe69b"), 2)); p.drawEllipse(center, 13, 13);
        p.drawLine(center + QPointF(-22, 0), center + QPointF(22, 0));
        p.drawLine(center + QPointF(0, -22), center + QPointF(0, 22));
    }
    if (m_showTargets) {
        const QColor colors[] {QColor("#75efb8"), QColor("#ffb660"), QColor("#dba3ff")};
        const QStringList names {"A", "B1", "B2"};
        for (int i = 0; i < 3; ++i) {
            const QPointF center = point(m_targets[i]);
            p.setPen(QPen(colors[i], i == m_active ? 3 : 2)); p.setBrush(QColor(0, 0, 0, 130));
            p.drawEllipse(center, 17, 17); p.drawText(QRectF(center.x() - 16, center.y() - 12, 32, 24), Qt::AlignCenter, names[i]);
        }
    }
    p.setPen(QPen(QColor("#f8ce75"), 2));
    for (int i = 0; i < m_calibration.size(); ++i) {
        const auto n = m_calibration[i].normalizedVideoPoint;
        QPointF center(box.left() + box.width() * n.x(), box.top() + box.height() * n.y());
        p.drawEllipse(center, 6, 6); p.drawText(center + QPointF(8, -8), QString::number(i + 1));
    }
}
void FieldView::moveTarget(const QPointF &position, bool final)
{
    const auto box = imageRect();
    const QPointF normalized(std::clamp((position.x() - box.x()) / box.width(), 0.0, 1.0),
        std::clamp((position.y() - box.y()) / box.height(), 0.0, 1.0));
    const auto direction = unprojectVideoPoint(normalized, m_camera);
    m_targets[m_active] = direction;
    emit targetMoved(m_active, direction.azimuthDegrees, direction.elevationDegrees, final); update();
}
void FieldView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !imageRect().contains(event->position())) return;
    if (m_showTargets) { m_dragging = true; moveTarget(event->position(), false); }
    else {
        const auto box = imageRect();
        emit pointClicked(QPointF((event->position().x() - box.x()) / box.width(), (event->position().y() - box.y()) / box.height()));
    }
}
void FieldView::mouseMoveEvent(QMouseEvent *event) { if (m_dragging) moveTarget(event->position(), false); }
void FieldView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragging && event->button() == Qt::LeftButton) { m_dragging = false; moveTarget(event->position(), true); }
}

} // namespace MicArray::Array128
