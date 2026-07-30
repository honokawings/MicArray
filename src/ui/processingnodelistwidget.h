#pragma once

#include "media/noisereductiontypes.h"

#include <QListWidget>

namespace MicArray {

class ProcessingNodeListWidget : public QListWidget
{
    Q_OBJECT

public:
    static constexpr int SuppressionNodeId = -1;

    explicit ProcessingNodeListWidget(QWidget *parent = nullptr);

    void setPostProcessingOrder(
        const QVector<NoisePostProcessingNode> &order);
    QVector<NoisePostProcessingNode> postProcessingOrder() const;
    int nodeIdAtRow(int row) const;
    void setNodeSummary(int nodeId,
                        bool enabled,
                        const QString &summary);

signals:
    void postProcessingOrderChanged(
        const QVector<NoisePostProcessingNode> &order);
    void currentNodeChanged(int nodeId);

protected:
    void dropEvent(QDropEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    static QString nodeName(int nodeId);
    QListWidgetItem *createItem(int nodeId);
    void refreshItemTexts();
    void restoreFixedFirstNode();
    void updateItemSizes();
};

} // namespace MicArray
