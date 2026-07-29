#pragma once

#include "media/noisereductiontypes.h"
#include "ui/videopreviewwidget.h"

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

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void drawTarget(QPainter &painter,
                    const QRectF &videoRectangle,
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
    bool m_markersVisible = true;
};

} // namespace MicArray
