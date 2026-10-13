#include "views.h"
#include "analysis/directionprojection.h"
#include "analysis/radix2fft.h"

#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace MicArray::Array128 {

ChannelGrid::ChannelGrid(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(480, 256);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(QStringLiteral("128 通道电平矩阵"));
    setToolTip(QStringLiteral("DATA/LR 索引排列，不代表物理坐标。点击或方向键选择通道，Home/End 跳至首末通道。× 为已屏蔽通道，保留电平但不参与质量阻断。停止或超时后，灰色数值仅为历史电平。"));
}
void ChannelGrid::setData(const PipelineSnapshot &snapshot, int selected)
{
    m_quality = snapshot.quality; m_selected = std::clamp(selected, 0, Channels - 1); m_hasData = !snapshot.latestHeader.isEmpty();
    m_disabled = snapshot.disabledProcessingChannels;
    m_fresh = snapshot.listening && snapshot.lastPacketAgeMs >= 0 && snapshot.lastPacketAgeMs < 1500;
    setAccessibleDescription(QStringLiteral("当前通道 %1，%2").arg(m_selected).arg(!m_hasData ? QStringLiteral("等待数据") : m_fresh ? QStringLiteral("实时电平") : QStringLiteral("历史电平")));
    update();
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
        if (!m_hasData || !m_fresh) color = QColor("#283744");
        if (m_fresh && q.clipped) color = QColor("#a44238");
        if (m_disabled[ch]) color = QColor("#283744");
        p.setBrush(color);
        p.setPen(QPen(ch == m_selected ? QColor("#ffe18d") : QColor("#304456"), ch == m_selected ? 2.5 : 0.7));
        p.drawRoundedRect(box, 4, 4);
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, QStringLiteral("%1%2\n%3").arg(ch).arg(m_disabled[ch] ? QStringLiteral(" ×") : QString())
            .arg(m_hasData ? QString::number(q.rmsDb, 'f', 0) : QStringLiteral("—")));
    }
    if (hasFocus()) { p.setBrush(Qt::NoBrush); p.setPen(QPen(QColor("#46bea5"), 2)); p.drawRect(rect().adjusted(1, 1, -1, -1)); }
}
void ChannelGrid::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    const int col = int(event->position().x() * 16 / width()), row = int(event->position().y() * 8 / height());
    if (col >= 0 && col < 16 && row >= 0 && row < 8) { setFocus(); m_selected = row * 16 + col; emit channelSelected(m_selected); update(); }
}

void ChannelGrid::keyPressEvent(QKeyEvent *event)
{
    int channel = m_selected;
    switch (event->key()) {
    case Qt::Key_Left: --channel; break;
    case Qt::Key_Right: ++channel; break;
    case Qt::Key_Up: channel -= 16; break;
    case Qt::Key_Down: channel += 16; break;
    case Qt::Key_Home: channel = 0; break;
    case Qt::Key_End: channel = Channels - 1; break;
    default: QWidget::keyPressEvent(event); return;
    }
    m_selected = std::clamp(channel, 0, Channels - 1); emit channelSelected(m_selected); update();
}

WaveView::WaveView(QWidget *parent) : QWidget(parent) { setMinimumHeight(80); }
void WaveView::setSamples(const QVector<float> &samples, bool current) { m_samples = samples; m_current = current; update(); }
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
    const QString title = m_spectrum ? QStringLiteral("幅度谱 0–24 kHz / dBFS") : QStringLiteral("通道波形 · 自动缩放（不改变信号）");
    p.drawText(8, 16, title + (m_current ? QString() : QStringLiteral(" · 历史快照")));
}

FieldView::FieldView(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(400, 260); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
}
void FieldView::setImage(const QImage &image) { m_image = image; update(); }
void FieldView::setFusionDetections(const QVector<FusionDetection> &detections) { m_fusion = detections; update(); }
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
    const bool trusted = result.trusted || std::any_of(result.sources.cbegin(), result.sources.cend(), [](const auto &source) { return source.trusted; });
    for (int y = 0; y < result.rows; ++y) for (int x = 0; x < result.columns; ++x) {
        const double value = std::pow(std::clamp(double(result.heat[y * result.columns + x]), 0.0, 1.0), focus);
        const double mapped = trusted ? value : value * 0.38;
        QColor color = QColor::fromHsvF((1 - mapped) * 0.67, 0.90, 1.0, 0.05 + value * (trusted ? 0.50 : 0.23));
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
        p.setPen(QColor("#9eb8cc")); p.drawText(box.adjusted(8, 8, -8, -8), Qt::AlignTop | Qt::AlignLeft, QStringLiteral("无视频 · 方向网格\n可打开 USB 视频或导入图像"));
    }
    if (!m_heat.isNull()) p.drawImage(box, m_heat);
    auto point = [&](const ScanDirection &d) {
        SoundSourceDirection s; s.azimuthDegrees = d.azimuthDegrees; s.elevationDegrees = d.elevationDegrees;
        const auto projection = projectDirection(s, m_camera);
        if (!projection.inView) return QPointF(-1000, -1000);
        return QPointF(box.x() + projection.normalizedPoint.x() * box.width(), box.y() + projection.normalizedPoint.y() * box.height());
    };
    auto drawSource = [&](const ScanDirection &direction, const QString &name) {
        const QPointF center = point(direction);
        p.setPen(QPen(QColor("#ffe69b"), 2)); p.drawEllipse(center, 13, 13);
        p.drawLine(center + QPointF(-22, 0), center + QPointF(22, 0));
        p.drawLine(center + QPointF(0, -22), center + QPointF(0, 22));
        p.drawText(center + QPointF(16, -16), name);
    };
    if (m_result.sources.isEmpty() && m_result.trusted) drawSource(m_result.direction, "S1");
    for (int i = 0; i < m_result.sources.size(); ++i)
        if (m_result.sources[i].trusted) drawSource(m_result.sources[i].direction, QStringLiteral("S%1").arg(i + 1));
    p.save(); p.setClipRect(box); p.setBrush(Qt::NoBrush);
    for (int i = 0; i < m_fusion.size(); ++i) {
        const auto &detection = m_fusion[i];
        const QRectF bounds(box.x() + detection.box.x() * box.width(), box.y() + detection.box.y() * box.height(),
            detection.box.width() * box.width(), detection.box.height() * box.height());
        const QPointF source(box.x() + detection.sourcePoint.x() * box.width(), box.y() + detection.sourcePoint.y() * box.height());
        p.setPen(QPen(QColor("#6ef6fb"), 2, Qt::DashLine)); p.drawRect(bounds);
        p.setPen(QPen(QColor("#6ef6fb"), 2)); p.drawEllipse(source, 5, 5);
        p.drawText(source + QPointF(8, 18), QStringLiteral("Y%1 · 仿真模型").arg(i + 1));
    }
    p.restore();
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
    if (hasFocus()) { p.setBrush(Qt::NoBrush); p.setPen(QPen(QColor("#46bea5"), 2)); p.drawRect(rect().adjusted(1, 1, -1, -1)); }
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
    setFocus();
    if (m_showTargets) {
        // A click on an existing marker selects that marker, not the previously
        // selected target. Empty-space clicks still reposition the active one.
        const auto box = imageRect(); double nearest = 23; const int previouslyActive = m_active;
        for (int offset = 0; offset < 3; ++offset) {
            const int target = (previouslyActive + offset) % 3;
            SoundSourceDirection direction; direction.azimuthDegrees = m_targets[target].azimuthDegrees; direction.elevationDegrees = m_targets[target].elevationDegrees;
            const auto projection = projectDirection(direction, m_camera);
            if (!projection.inView) continue;
            const QPointF center(box.x() + box.width() * projection.normalizedPoint.x(), box.y() + box.height() * projection.normalizedPoint.y());
            const double distance = QLineF(center, event->position()).length();
            if (distance < nearest) { nearest = distance; m_active = target; }
        }
        m_dragging = true; moveTarget(event->position(), false);
    }
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

void FieldView::keyPressEvent(QKeyEvent *event)
{
    if (!m_showTargets) { QWidget::keyPressEvent(event); return; }
    auto direction = m_targets[m_active];
    const double step = event->modifiers().testFlag(Qt::ShiftModifier) ? 1.0 : .1;
    switch (event->key()) {
    case Qt::Key_Left: direction.azimuthDegrees -= step; break;
    case Qt::Key_Right: direction.azimuthDegrees += step; break;
    case Qt::Key_Up: direction.elevationDegrees += step; break;
    case Qt::Key_Down: direction.elevationDegrees -= step; break;
    default: QWidget::keyPressEvent(event); return;
    }
    direction.azimuthDegrees = std::clamp(direction.azimuthDegrees, -89.0, 89.0);
    direction.elevationDegrees = std::clamp(direction.elevationDegrees, -89.0, 89.0);
    m_targets[m_active] = direction;
    emit targetMoved(m_active, direction.azimuthDegrees, direction.elevationDegrees, true); update();
}

} // namespace MicArray::Array128
