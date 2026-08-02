#include "ui/livelocalizationview.h"

#include "analysis/directionprojection.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace MicArray {

namespace {

constexpr double UntrustedHeatmapScale = 0.35;

} // namespace

LiveLocalizationView::LiveLocalizationView(QWidget *parent)
    : VideoPreviewWidget(parent)
{
    setObjectName(QStringLiteral("liveLocalizationView"));
}

void LiveLocalizationView::setCameraView(const CameraViewSettings &camera)
{
    m_camera = camera;
    invalidateHeatmapCache();
    update();
}

void LiveLocalizationView::setDirections(
    const QVector<SoundSourceDirection> &directions)
{
    m_directions = directions;
    update();
}

void LiveLocalizationView::setSpatialResponseActive(bool active)
{
    if (m_spatialResponseActive == active) {
        return;
    }
    m_spatialResponseActive = active;
    if (!active) {
        m_response = {};
        invalidateHeatmapCache();
    }
    update();
}

void LiveLocalizationView::setExpectedSpatialResponseGeneration(
    quint64 generation)
{
    if (m_expectedSpatialResponseGeneration == generation) {
        return;
    }
    m_expectedSpatialResponseGeneration = generation;
    m_response = {};
    invalidateHeatmapCache();
    update();
}

void LiveLocalizationView::setSpatialResponse(const LiveSpatialResponse &response)
{
    if (!m_spatialResponseActive
        || response.generation
        != m_expectedSpatialResponseGeneration) {
        return;
    }
    m_response = response;
    invalidateHeatmapCache();
    update();
}

void LiveLocalizationView::setDisplayMode(const QString &mode)
{
    m_displayMode = mode;
    update();
}

void LiveLocalizationView::setHeatmapOpacity(double opacity)
{
    m_heatmapOpacity = std::clamp(opacity, 0.0, 1.0);
    update();
}

void LiveLocalizationView::setHeatmapFocusExponent(double exponent)
{
    const double bounded = std::isfinite(exponent)
        ? std::clamp(exponent, 0.5, 4.0)
        : 1.5;
    if (qFuzzyCompare(m_heatmapFocusExponent, bounded)) {
        return;
    }
    m_heatmapFocusExponent = bounded;
    invalidateHeatmapCache();
    update();
}

void LiveLocalizationView::setCalibrationMode(bool enabled)
{
    m_calibrationMode = enabled;
    update();
}

void LiveLocalizationView::setCalibrationSamples(
    const QVector<VideoCalibrationSample> &samples,
    const QVector<QPointF> &predictedPoints)
{
    m_calibrationSamples = samples;
    m_predictedPoints = predictedPoints;
    update();
}

void LiveLocalizationView::setStatusText(const QString &text)
{
    m_statusText = text;
    update();
}

void LiveLocalizationView::mousePressEvent(QMouseEvent *event)
{
    if (m_calibrationMode && event->button() == Qt::LeftButton) {
        const auto point = normalizedVideoPoint(event->position());
        if (point.has_value()) {
            emit calibrationPointClicked(*point);
            event->accept();
            return;
        }
    }
    VideoPreviewWidget::mousePressEvent(event);
}

void LiveLocalizationView::paintEvent(QPaintEvent *event)
{
    VideoPreviewWidget::paintEvent(event);
    const QRectF target = videoRect();
    if (target.isEmpty()) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    if (m_displayMode == QStringLiteral("heatmap")) {
        const QImage heatmap = heatmapImage();
        if (!heatmap.isNull()) {
            painter.setOpacity(m_heatmapOpacity);
            painter.drawImage(target, heatmap);
            painter.setOpacity(1.0);
        }
    }
    for (const SoundSourceDirection &direction : m_directions) {
        if (m_displayMode != QStringLiteral("marker")) {
            break;
        }
        const ProjectionResult projected = projectDirection(direction, m_camera);
        if (!projected.valid || !projected.inFront) {
            continue;
        }
        const QPointF normalized = projected.inView
            ? projected.normalizedPoint : projected.edgePoint;
        const QPointF point(target.left() + normalized.x() * target.width(),
                            target.top() + normalized.y() * target.height());
        painter.setPen(QPen(Qt::white, 2));
        painter.setBrush(QColor(255, 65, 65, 210));
        painter.drawEllipse(point, 9, 9);
        painter.drawLine(point + QPointF(-15, 0), point + QPointF(15, 0));
        painter.drawLine(point + QPointF(0, -15), point + QPointF(0, 15));
        painter.drawText(point + QPointF(13, -10),
                         QStringLiteral("%1° / %2°  %3%")
                             .arg(direction.azimuthDegrees, 0, 'f', 1)
                             .arg(direction.elevationDegrees, 0, 'f', 1)
                             .arg(direction.confidence * 100.0, 0, 'f', 0));
        break;
    }
    for (int index = 0; index < m_calibrationSamples.size(); ++index) {
        const QPointF normalized = m_calibrationSamples.at(index).normalizedVideoPoint;
        const QPointF point(target.left() + normalized.x() * target.width(),
                            target.top() + normalized.y() * target.height());
        painter.setPen(QPen(QColor(0, 220, 255), 2));
        painter.setBrush(QColor(0, 90, 120, 180));
        painter.drawEllipse(point, 7, 7);
        painter.drawText(point + QPointF(9, -7), QString::number(index + 1));
        if (index < m_predictedPoints.size()) {
            const QPointF predicted(target.left()
                                        + m_predictedPoints.at(index).x() * target.width(),
                                    target.top()
                                        + m_predictedPoints.at(index).y() * target.height());
            painter.setPen(QPen(QColor(255, 220, 70), 2));
            painter.drawLine(point, predicted);
            painter.drawRect(QRectF(predicted - QPointF(5, 5), QSizeF(10, 10)));
        }
    }
    if (!m_statusText.isEmpty()) {
        painter.setPen(Qt::white);
        painter.setBrush(QColor(0, 0, 0, 150));
        const QRectF label = painter.boundingRect(
            target.adjusted(12, 12, -12, -12),
            Qt::AlignLeft | Qt::AlignBottom, m_statusText);
        painter.drawRoundedRect(label.adjusted(-7, -4, 7, 4), 4, 4);
        painter.drawText(target.adjusted(12, 12, -12, -12),
                         Qt::AlignLeft | Qt::AlignBottom, m_statusText);
    }
}

void LiveLocalizationView::invalidateHeatmapCache()
{
    ++m_heatmapRevision;
    m_cachedHeatmap = {};
    m_cachedHeatmapSize = {};
}

QImage LiveLocalizationView::heatmapImage() const
{
    if (m_response.columns <= 0 || m_response.rows <= 0
        || m_response.normalizedValues.size()
            != m_response.columns * m_response.rows) {
        return {};
    }
    const QRectF target = videoRect();
    const int imageWidth = std::clamp(
        qRound(target.width()), 256, 640);
    const int imageHeight = std::clamp(
        qRound(target.height()), 144, 360);
    const QSize imageSize(imageWidth, imageHeight);
    if (m_cachedHeatmapRevision == m_heatmapRevision
        && m_cachedHeatmapSize == imageSize
        && !m_cachedHeatmap.isNull()) {
        return m_cachedHeatmap;
    }
    QImage image(imageWidth, imageHeight,
                 QImage::Format_ARGB32);

    const auto gridValue = [this](int column, int row) {
        return m_response.normalizedValues.at(
            row * m_response.columns + column);
    };
    const auto interpolatedValue =
        [this, &gridValue](double normalizedX,
                           double normalizedY) {
        const double gridX = std::clamp(
            normalizedX * m_response.columns - 0.5,
            0.0,
            static_cast<double>(m_response.columns - 1));
        const double gridY = std::clamp(
            normalizedY * m_response.rows - 0.5,
            0.0,
            static_cast<double>(m_response.rows - 1));
        const int left = static_cast<int>(
            std::floor(gridX));
        const int top = static_cast<int>(
            std::floor(gridY));
        const int right = std::min(
            left + 1, m_response.columns - 1);
        const int bottom = std::min(
            top + 1, m_response.rows - 1);
        const double horizontal = gridX - left;
        const double vertical = gridY - top;
        const double topValue =
            gridValue(left, top) * (1.0 - horizontal)
            + gridValue(right, top) * horizontal;
        const double bottomValue =
            gridValue(left, bottom) * (1.0 - horizontal)
            + gridValue(right, bottom) * horizontal;
        return topValue * (1.0 - vertical)
            + bottomValue * vertical;
    };

    const float gridMaximum = *std::max_element(
        m_response.normalizedValues.cbegin(),
        m_response.normalizedValues.cend());
    QPointF peakPointPixels;
    QPointF azimuthBasisPixels;
    QPointF elevationBasisPixels;
    bool renderRefinedPeak =
        m_response.trustedDirection
        && m_response.hasRefinedPeak
        && m_response.reliability > 0.0
        && std::isfinite(
            m_response.azimuthStandardDeviationDegrees)
        && std::isfinite(
            m_response.elevationStandardDeviationDegrees)
        && std::isfinite(
            m_response.azimuthElevationCorrelation)
        && m_response.azimuthStandardDeviationDegrees > 0.0
        && m_response.elevationStandardDeviationDegrees > 0.0
        && std::abs(
            m_response.azimuthElevationCorrelation) < 1.0
        && gridMaximum > 0.0f;
    if (renderRefinedPeak) {
        const auto projected =
            [this](double azimuth, double elevation) {
            SoundSourceDirection direction;
            direction.azimuthDegrees = azimuth;
            direction.elevationDegrees = elevation;
            return projectDirection(direction, m_camera);
        };
        const ProjectionResult center = projected(
            m_response.refinedPeak.azimuthDegrees,
            m_response.refinedPeak.elevationDegrees);
        const ProjectionResult azimuthOffset = projected(
            m_response.refinedPeak.azimuthDegrees
                + m_response.azimuthStandardDeviationDegrees,
            m_response.refinedPeak.elevationDegrees);
        const ProjectionResult elevationOffset = projected(
            m_response.refinedPeak.azimuthDegrees,
            m_response.refinedPeak.elevationDegrees
                + m_response.elevationStandardDeviationDegrees);
        renderRefinedPeak = center.valid && center.inFront
            && center.inView
            && azimuthOffset.valid && azimuthOffset.inFront
            && elevationOffset.valid
            && elevationOffset.inFront;
        if (renderRefinedPeak) {
            peakPointPixels = {
                center.normalizedPoint.x() * imageWidth,
                center.normalizedPoint.y() * imageHeight,
            };
            azimuthBasisPixels = {
                (azimuthOffset.normalizedPoint.x()
                 - center.normalizedPoint.x()) * imageWidth,
                (azimuthOffset.normalizedPoint.y()
                 - center.normalizedPoint.y()) * imageHeight,
            };
            elevationBasisPixels = {
                (elevationOffset.normalizedPoint.x()
                 - center.normalizedPoint.x()) * imageWidth,
                (elevationOffset.normalizedPoint.y()
                 - center.normalizedPoint.y()) * imageHeight,
            };
            const auto ensureVisibleBasis = [](QPointF basis) {
                const double length = std::hypot(
                    basis.x(), basis.y());
                if (length < 1.0 && length > 1.0e-9) {
                    basis *= 1.0 / length;
                }
                return basis;
            };
            azimuthBasisPixels =
                ensureVisibleBasis(azimuthBasisPixels);
            elevationBasisPixels =
                ensureVisibleBasis(elevationBasisPixels);
            const double determinant =
                azimuthBasisPixels.x()
                    * elevationBasisPixels.y()
                - elevationBasisPixels.x()
                    * azimuthBasisPixels.y();
            renderRefinedPeak =
                std::isfinite(determinant)
                && std::abs(determinant) > 1.0e-6;
        }
    }

    const auto colorForValue = [this](double value) {
        value = std::clamp(value, 0.0, 1.0);
        value = std::pow(value, m_heatmapFocusExponent);
        if (!m_response.trustedDirection) {
            value *= UntrustedHeatmapScale;
        }
        const int hue = static_cast<int>(
            (1.0 - value) * 240.0);
        return QColor::fromHsv(hue, 255, 255, 230).rgba();
    };
    for (int row = 0; row < imageHeight; ++row) {
        auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(row));
        const double normalizedY =
            (row + 0.5) / imageHeight;
        for (int column = 0; column < imageWidth; ++column) {
            const double normalizedX =
                (column + 0.5) / imageWidth;
            const double value = interpolatedValue(
                normalizedX, normalizedY);
            pixels[column] = colorForValue(value);
        }
    }

    if (renderRefinedPeak) {
        const double correlation =
            m_response.azimuthElevationCorrelation;
        const double correlationDenominator =
            1.0 - correlation * correlation;
        const double determinant =
            azimuthBasisPixels.x()
                * elevationBasisPixels.y()
            - elevationBasisPixels.x()
                * azimuthBasisPixels.y();
        const double horizontalRadius = 3.0 * std::sqrt(
            std::max(
                0.0,
                azimuthBasisPixels.x()
                    * azimuthBasisPixels.x()
                + elevationBasisPixels.x()
                    * elevationBasisPixels.x()
                + 2.0 * correlation
                    * azimuthBasisPixels.x()
                    * elevationBasisPixels.x()));
        const double verticalRadius = 3.0 * std::sqrt(
            std::max(
                0.0,
                azimuthBasisPixels.y()
                    * azimuthBasisPixels.y()
                + elevationBasisPixels.y()
                    * elevationBasisPixels.y()
                + 2.0 * correlation
                    * azimuthBasisPixels.y()
                    * elevationBasisPixels.y()));
        const int left = std::clamp(
            static_cast<int>(std::floor(
                peakPointPixels.x() - horizontalRadius)),
            0, imageWidth - 1);
        const int right = std::clamp(
            static_cast<int>(std::ceil(
                peakPointPixels.x() + horizontalRadius)),
            0, imageWidth - 1);
        const int top = std::clamp(
            static_cast<int>(std::floor(
                peakPointPixels.y() - verticalRadius)),
            0, imageHeight - 1);
        const int bottom = std::clamp(
            static_cast<int>(std::ceil(
                peakPointPixels.y() + verticalRadius)),
            0, imageHeight - 1);
        for (int row = top; row <= bottom; ++row) {
            auto *pixels = reinterpret_cast<QRgb *>(
                image.scanLine(row));
            const double normalizedY =
                (row + 0.5) / imageHeight;
            for (int column = left;
                 column <= right;
                 ++column) {
                const QPointF delta(
                    column + 0.5 - peakPointPixels.x(),
                    row + 0.5 - peakPointPixels.y());
                const double azimuthCoordinate = (
                    elevationBasisPixels.y() * delta.x()
                    - elevationBasisPixels.x() * delta.y())
                    / determinant;
                const double elevationCoordinate = (
                    -azimuthBasisPixels.y() * delta.x()
                    + azimuthBasisPixels.x() * delta.y())
                    / determinant;
                const double radiusSquared =
                    (azimuthCoordinate * azimuthCoordinate
                     - 2.0 * correlation
                         * azimuthCoordinate
                         * elevationCoordinate
                     + elevationCoordinate
                         * elevationCoordinate)
                    / correlationDenominator;
                if (radiusSquared > 9.0) {
                    continue;
                }
                const double normalizedX =
                    (column + 0.5) / imageWidth;
                const double base = interpolatedValue(
                    normalizedX, normalizedY);
                const double localLikelihood =
                    gridMaximum * std::exp(
                        -0.5 * radiusSquared);
                pixels[column] = colorForValue(
                    std::max(base, localLikelihood));
            }
        }
    }
    m_cachedHeatmapRevision = m_heatmapRevision;
    m_cachedHeatmapSize = imageSize;
    m_cachedHeatmap = image;
    return m_cachedHeatmap;
}

} // namespace MicArray
