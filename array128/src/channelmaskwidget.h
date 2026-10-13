#pragma once

#include <QWidget>
#include <array>

class QLabel;
class QLineEdit;
class QTableWidget;

namespace MicArray::Array128 {

using ChannelMask = std::array<bool, 128>;
bool parseChannelMask(const QString &text, ChannelMask *mask, QString *error);
QString channelMaskText(const ChannelMask &mask);

class ChannelMaskWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit ChannelMaskWidget(QWidget *parent = nullptr);
    ChannelMask mask() const { return m_mask; }
    void setMask(const ChannelMask &mask);
signals:
    void maskChanged();
    void inputRejected(const QString &reason);
private:
    void updateSummary();
    void setSelected(bool disabled);
    ChannelMask m_mask {};
    QTableWidget *m_table = nullptr;
    QLineEdit *m_list = nullptr;
    QLabel *m_summary = nullptr;
};

} // namespace MicArray::Array128
