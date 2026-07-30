#pragma once

#include "media/noisereductiontypes.h"
#include "ui/videopreviewwidget.h"

#include <optional>

class QColor;
class QPainter;

namespace MicArray {

class NoiseReductionVideoWidget : public VideoPreviewWidget
{
    Q_OBJECT

public:
    explicit NoiseReductionVideoWidget(QWidget *parent = nullptr);

    void setCameraView(const CameraViewSettings &camera);
    void setControlMapping(const DirectionControlMapping &mapping);
    void setDirectionTargets(const ContinuousDirectionTarget &enhancedA,
                             const ContinuousDirectionTarget &suppressionB1,
                             const ContinuousDirectionTarget &suppressionB2);
    void setCurrentSelection(NoiseDirectionSelection selection);
    void setMarkersVisible(bool visible);
    bool markersVisible() const { return m_markersVisible; }

signals:
    void videoPointClicked(const QPointF &normalizedPoint);
    void directionDragStarted(NoiseDirectionSelection selection);
    void directionDragged(NoiseDirectionSelection selection,
                          const QPointF &normalizedPoint);
    void directionDragFinished(NoiseDirectionSelection selection,
                               const QPointF &normalizedPoint);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    std::optional<QPointF> targetPoint(
        const ContinuousDirectionTarget &target) const;
    std::optional<NoiseDirectionSelection> hitTarget(
        const QPointF &point) const;
    std::optional<QPointF> clampedNormalizedPoint(
        const QPointF &point) const;
    void drawTarget(QPainter &painter,
                    const QString &name,
                    const QColor &color,
                    const ContinuousDirectionTarget &target,
                    NoiseDirectionSelection selection);

    CameraViewSettings m_camera;
    DirectionControlMapping m_mapping;
    ContinuousDirectionTarget m_enhancedA;
    ContinuousDirectionTarget m_suppressionB1;
    ContinuousDirectionTarget m_suppressionB2;
    NoiseDirectionSelection m_selection =
        NoiseDirectionSelection::EnhancedA;
    std::optional<NoiseDirectionSelection> m_dragSelection;
    bool m_markersVisible = true;
};

} // namespace MicArray
