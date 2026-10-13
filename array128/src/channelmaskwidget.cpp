#include "channelmaskwidget.h"

#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace MicArray::Array128 {

bool parseChannelMask(const QString &text, ChannelMask *mask, QString *error)
{
    if (!mask) return false;
    ChannelMask parsed {};
    const auto tokens = text.trimmed().split(QRegularExpression(QStringLiteral("[,，;；\\s]+")), Qt::SkipEmptyParts);
    const QRegularExpression token(QStringLiteral("^([0-9]{1,3})(?:-([0-9]{1,3}))?$"));
    for (const auto &part : tokens) {
        const auto match = token.match(part);
        const int first = match.captured(1).toInt();
        const int last = match.captured(2).isEmpty() ? first : match.captured(2).toInt();
        if (!match.hasMatch() || first > 127 || last > 127 || last < first) {
            if (error) *error = QStringLiteral("无效通道“%1”：使用 0–127 编号或递增范围，例如 0,7,16-23；原屏蔽不变。").arg(part);
            return false;
        }
        for (int ch = first; ch <= last; ++ch) parsed[ch] = true;
    }
    *mask = parsed;
    if (error) error->clear();
    return true;
}

QString channelMaskText(const ChannelMask &mask)
{
    QStringList parts;
    for (int ch = 0; ch < 128; ++ch) {
        if (!mask[ch]) continue;
        const int first = ch;
        while (ch + 1 < 128 && mask[ch + 1]) ++ch;
        parts.append(first == ch ? QString::number(ch) : QStringLiteral("%1-%2").arg(first).arg(ch));
    }
    return parts.join(',');
}

ChannelMaskWidget::ChannelMaskWidget(QWidget *parent) : QWidget(parent)
{
    setObjectName("channelMaskPanel");
    auto *layout = new QVBoxLayout(this);
    auto *note = new QLabel(QStringLiteral("勾选 = 屏蔽。排除指定通道的声源定位、软件波束形成和削顶质量阻断；全局时序/打包故障不受屏蔽影响。原始接收、监听、诊断统计、录制与自测审计保持不变。"));
    note->setWordWrap(true); layout->addWidget(note);
    auto *entry = new QHBoxLayout;
    m_list = new QLineEdit; m_list->setObjectName("disabledChannelList");
    m_list->setPlaceholderText(QStringLiteral("例如 0,7,16-23；空列表表示全部启用"));
    m_list->setAccessibleName(QStringLiteral("屏蔽通道编号列表"));
    auto *apply = new QPushButton(QStringLiteral("应用列表")); apply->setObjectName("applyChannelMask");
    entry->addWidget(new QLabel(QStringLiteral("屏蔽列表"))); entry->addWidget(m_list, 1); entry->addWidget(apply); layout->addLayout(entry);
    m_table = new QTableWidget(8, 16); m_table->setObjectName("channelMaskTable");
    m_table->setAccessibleName(QStringLiteral("失效通道屏蔽；勾选为屏蔽"));
    m_table->horizontalHeader()->hide(); m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setMinimumSectionSize(45);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setMinimumHeight(256);
    for (int ch = 0; ch < 128; ++ch) {
        auto *item = new QTableWidgetItem(QStringLiteral("%1").arg(ch, 3, 10, QChar('0')));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        item->setToolTip(QStringLiteral("CH%1 · 勾选后不参与定位与波束形成").arg(ch));
        m_table->setItem(ch / 16, ch % 16, item);
    }
    layout->addWidget(m_table, 1);
    auto *actions = new QHBoxLayout;
    for (bool disabled : {true, false}) {
        auto *button = new QPushButton(disabled ? QStringLiteral("屏蔽选中") : QStringLiteral("恢复选中"));
        button->setObjectName(disabled ? "disableSelectedChannels" : "enableSelectedChannels");
        actions->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, disabled] { setSelected(disabled); });
    }
    auto *clear = new QPushButton(QStringLiteral("恢复全部")); clear->setObjectName("clearChannelMask"); actions->addWidget(clear); actions->addStretch();
    m_summary = new QLabel; m_summary->setObjectName("channelMaskSummary"); actions->addWidget(m_summary); layout->addLayout(actions);
    connect(clear, &QPushButton::clicked, this, [this] { setMask({}); });
    connect(apply, &QPushButton::clicked, this, [this] {
        ChannelMask next; QString error;
        if (!parseChannelMask(m_list->text(), &next, &error)) { emit inputRejected(error); return; }
        setMask(next); updateSummary();
    });
    connect(m_list, &QLineEdit::returnPressed, apply, &QPushButton::click);
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        auto next = m_mask; next[item->row() * 16 + item->column()] = item->checkState() == Qt::Checked; setMask(next);
    });
    updateSummary();
}

void ChannelMaskWidget::setMask(const ChannelMask &mask)
{
    const bool changed = mask != m_mask;
    m_mask = mask;
    const QSignalBlocker blocker(m_table);
    for (int ch = 0; ch < 128; ++ch) m_table->item(ch / 16, ch % 16)->setCheckState(mask[ch] ? Qt::Checked : Qt::Unchecked);
    updateSummary();
    if (changed) emit maskChanged();
}

void ChannelMaskWidget::updateSummary()
{
    m_list->setText(channelMaskText(m_mask));
    const int disabled = int(std::count(m_mask.cbegin(), m_mask.cend(), true));
    m_summary->setText(QStringLiteral("屏蔽 %1 / 128 · 启用 %2").arg(disabled).arg(128 - disabled));
}

void ChannelMaskWidget::setSelected(bool disabled)
{
    const auto selected = m_table->selectedItems();
    if (selected.isEmpty()) { emit inputRejected(QStringLiteral("先选择一个或多个通道；Ctrl/Shift 可多选，空格可切换勾选。")); return; }
    auto next = m_mask;
    for (const auto *item : selected) next[item->row() * 16 + item->column()] = disabled;
    setMask(next);
}

} // namespace MicArray::Array128
