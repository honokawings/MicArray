#include "ui/processingnodelistwidget.h"

#include <QDropEvent>
#include <QResizeEvent>

#include <algorithm>

namespace MicArray {
namespace {

constexpr int NodeIdRole = Qt::UserRole;
constexpr int EnabledRole = Qt::UserRole + 1;
constexpr int SummaryRole = Qt::UserRole + 2;

} // namespace

ProcessingNodeListWidget::ProcessingNodeListWidget(QWidget *parent)
    : QListWidget(parent)
{
    setObjectName(QStringLiteral("noiseReductionProcessingNodeList"));
    setSelectionMode(QAbstractItemView::SingleSelection);
    setDragDropMode(QAbstractItemView::InternalMove);
    setDefaultDropAction(Qt::MoveAction);
    setDropIndicatorShown(true);
    setFlow(QListView::LeftToRight);
    setWrapping(false);
    setResizeMode(QListView::Adjust);
    setUniformItemSizes(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setWordWrap(false);
    setSpacing(5);
    setFixedHeight(88);
    setStyleSheet(QStringLiteral(
        "QListWidget { border: none; background: transparent; }"
        "QListWidget::item {"
        " border: 1px solid palette(mid);"
        " border-radius: 8px;"
        " padding: 9px 12px;"
        " margin: 1px;"
        " background: palette(base);"
        "}"
        "QListWidget::item:selected {"
        " border: 2px solid palette(highlight);"
        " background: palette(alternate-base);"
        " color: palette(text);"
        "}"));
    connect(this, &QListWidget::currentRowChanged,
            this, [this](int row) {
                emit currentNodeChanged(nodeIdAtRow(row));
            });
    setPostProcessingOrder({});
    setCurrentRow(0);
}

void ProcessingNodeListWidget::setPostProcessingOrder(
    const QVector<NoisePostProcessingNode> &order)
{
    const int selectedNode = nodeIdAtRow(currentRow());
    clear();
    addItem(createItem(SuppressionNodeId));
    for (NoisePostProcessingNode node
         : normalizedPostProcessingOrder(order)) {
        addItem(createItem(static_cast<int>(node)));
    }
    refreshItemTexts();
    updateItemSizes();
    for (int row = 0; row < count(); ++row) {
        if (nodeIdAtRow(row) == selectedNode) {
            setCurrentRow(row);
            return;
        }
    }
    setCurrentRow(0);
}

QVector<NoisePostProcessingNode>
ProcessingNodeListWidget::postProcessingOrder() const
{
    QVector<NoisePostProcessingNode> order;
    for (int row = 1; row < count(); ++row) {
        const int nodeId = nodeIdAtRow(row);
        if (nodeId >= static_cast<int>(
                          NoisePostProcessingNode::BandPass)
            && nodeId <= static_cast<int>(
                             NoisePostProcessingNode::
                                 AutomaticGainControl)) {
            order.append(
                static_cast<NoisePostProcessingNode>(nodeId));
        }
    }
    return normalizedPostProcessingOrder(order);
}

int ProcessingNodeListWidget::nodeIdAtRow(int row) const
{
    const QListWidgetItem *node = item(row);
    return node != nullptr
        ? node->data(NodeIdRole).toInt() : SuppressionNodeId;
}

void ProcessingNodeListWidget::setNodeSummary(
    int nodeId, bool enabled, const QString &summary)
{
    for (int row = 0; row < count(); ++row) {
        QListWidgetItem *node = item(row);
        if (nodeIdAtRow(row) != nodeId) {
            continue;
        }
        node->setData(EnabledRole, enabled);
        node->setData(SummaryRole, summary);
        refreshItemTexts();
        return;
    }
}

void ProcessingNodeListWidget::dropEvent(QDropEvent *event)
{
    const QVector<NoisePostProcessingNode> before =
        postProcessingOrder();
    QListWidget::dropEvent(event);
    restoreFixedFirstNode();
    refreshItemTexts();
    updateItemSizes();
    const QVector<NoisePostProcessingNode> after =
        postProcessingOrder();
    if (after != before) {
        emit postProcessingOrderChanged(after);
    }
}

void ProcessingNodeListWidget::resizeEvent(QResizeEvent *event)
{
    QListWidget::resizeEvent(event);
    updateItemSizes();
}

QString ProcessingNodeListWidget::nodeName(int nodeId)
{
    if (nodeId == SuppressionNodeId) {
        return QStringLiteral("降噪抑制");
    }
    switch (static_cast<NoisePostProcessingNode>(nodeId)) {
    case NoisePostProcessingNode::BandPass:
        return QStringLiteral("带通滤波");
    case NoisePostProcessingNode::LogMmse:
        return QStringLiteral("LogMMSE");
    case NoisePostProcessingNode::AutomaticGainControl:
        return QStringLiteral("AGC");
    }
    return QStringLiteral("未知节点");
}

QListWidgetItem *ProcessingNodeListWidget::createItem(int nodeId)
{
    auto *node = new QListWidgetItem;
    node->setData(NodeIdRole, nodeId);
    node->setData(EnabledRole, false);
    node->setData(SummaryRole, QString());
    node->setTextAlignment(Qt::AlignCenter);
    if (nodeId == SuppressionNodeId) {
        node->setFlags(node->flags() & ~Qt::ItemIsDragEnabled);
    } else {
        node->setFlags(node->flags() | Qt::ItemIsDragEnabled);
    }
    return node;
}

void ProcessingNodeListWidget::refreshItemTexts()
{
    for (int row = 0; row < count(); ++row) {
        QListWidgetItem *node = item(row);
        const int nodeId = nodeIdAtRow(row);
        const bool enabled = node->data(EnabledRole).toBool();
        const QString summary = node->data(SummaryRole).toString();
        const QString state =
            enabled ? QStringLiteral("已启用")
                    : QStringLiteral("已旁路");
        const QString arrow =
            row + 1 < count() ? QStringLiteral("  →")
                              : QString();
        node->setText(
            QStringLiteral("%1  %2\n%3%4")
                .arg(nodeId == SuppressionNodeId
                         ? QStringLiteral("●")
                         : QStringLiteral("☰"),
                     nodeName(nodeId),
                     state,
                     arrow));
        node->setToolTip(
            summary.isEmpty()
                ? QStringLiteral("%1\n%2")
                      .arg(nodeName(nodeId), state)
                : QStringLiteral("%1\n%2 · %3")
                      .arg(nodeName(nodeId), state, summary));
    }
}

void ProcessingNodeListWidget::restoreFixedFirstNode()
{
    for (int row = 0; row < count(); ++row) {
        if (nodeIdAtRow(row) != SuppressionNodeId) {
            continue;
        }
        if (row != 0) {
            insertItem(0, takeItem(row));
        }
        item(0)->setFlags(
            item(0)->flags() & ~Qt::ItemIsDragEnabled);
        return;
    }
    insertItem(0, createItem(SuppressionNodeId));
}

void ProcessingNodeListWidget::updateItemSizes()
{
    if (count() <= 0 || viewport()->width() <= 0) {
        return;
    }
    const int availableWidth =
        viewport()->width() - 2 * spacing() * count();
    const int itemWidth =
        std::max(1, availableWidth / count());
    const int itemHeight =
        std::max(1, viewport()->height() - 2 * spacing());
    for (int row = 0; row < count(); ++row) {
        item(row)->setSizeHint(QSize(itemWidth, itemHeight));
    }
    scheduleDelayedItemsLayout();
}

} // namespace MicArray
