#include "sys3panel.h"

#include <QComboBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace MicArray::Array128 {
namespace {
class PortCombo final : public QComboBox
{
protected:
    void wheelEvent(QWheelEvent *event) override { event->ignore(); }
};
QTableWidget *table(const QStringList &headers)
{
    auto *widget = new QTableWidget(0, headers.size()); widget->setHorizontalHeaderLabels(headers);
    widget->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); widget->verticalHeader()->hide();
    widget->setEditTriggers(QAbstractItemView::NoEditTriggers); widget->setAlternatingRowColors(true);
    widget->setSelectionBehavior(QAbstractItemView::SelectRows); widget->verticalHeader()->setDefaultSectionSize(28);
    return widget;
}
void cell(QTableWidget *table, int row, int col, const QString &text)
{
    auto *item = table->item(row, col);
    if (!item) { item = new QTableWidgetItem; table->setItem(row, col, item); }
    if (item->text() != text) item->setText(text); item->setToolTip(text);
}
}

Sys3Panel::Sys3Panel(const QString &preferredPort, QWidget *parent) : QWidget(parent)
{
    setObjectName("sys3Panel");
    auto *layout = new QVBoxLayout(this); auto *connection = new QHBoxLayout;
    m_ports = new PortCombo; m_ports->setEditable(true); m_ports->setMinimumWidth(95); m_ports->setMaximumWidth(160); m_ports->setObjectName("sys3Port");
    m_ports->setEditText(preferredPort); refreshPorts();
    m_connect = new QPushButton(QStringLiteral("连接串口")); m_connect->setObjectName("sys3Connect");
    m_scan = new QPushButton(QStringLiteral("刷新端口"));
    connection->addWidget(new QLabel(QStringLiteral("端口"))); connection->addWidget(m_ports); connection->addWidget(m_connect); connection->addWidget(m_scan);
    connection->addWidget(new QLabel(QStringLiteral("115200 8N1 · 无流控 · UART 3.3 V"))); connection->addStretch();
    auto *saveLog = new QPushButton(QStringLiteral("导出日志")); connection->addWidget(saveLog); layout->addLayout(connection);
    m_status = new QLabel; m_status->setObjectName("sys3Status"); m_status->setWordWrap(true); layout->addWidget(m_status);
    m_freshness = new QLabel; m_freshness->setWordWrap(true); layout->addWidget(m_freshness);
    auto *commands = new QHBoxLayout;
    const QStringList names {QStringLiteral("读取状态 (s)"), QStringLiteral("状态别名 (h)"), QStringLiteral("系统复位 (r)"), QStringLiteral("切换自测 (t)"), QStringLiteral("清时钟历史 (c)")};
    for (int i = 0; i < 5; ++i) {
        const char command = "shrtc"[i]; auto *button = new QPushButton(names[i]); m_commands[i] = button;
        button->setObjectName(QString("sys3Command_%1").arg(QChar(command))); commands->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, command] { sendCommand(command); });
    }
    commands->addStretch(); layout->addLayout(commands);
    m_commandStatus = new QLabel; m_commandStatus->setWordWrap(true); layout->addWidget(m_commandStatus);
    auto *tabs = new QTabWidget; tabs->setObjectName("sys3DetailsTabs"); layout->addWidget(tabs, 1);
    m_details = table({QStringLiteral("参数"), QStringLiteral("设备快照值"), QStringLiteral("时效 / 所属域")});
    m_details->setObjectName("sys3DecodedTable"); tabs->addTab(m_details, QStringLiteral("状态参数"));
    m_words = table({QStringLiteral("索引"), QStringLiteral("字段"), QStringLiteral("HEX"), QStringLiteral("无符号值"), QStringLiteral("时效")});
    m_words->setObjectName("sys3RawTable"); m_words->setRowCount(34);
    m_words->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tabs->addTab(m_words, QStringLiteral("原始字"));
    m_log = new QPlainTextEdit; m_log->setReadOnly(true); m_log->setMaximumBlockCount(400); tabs->addTab(m_log, QStringLiteral("事件记录"));
    auto *note = new QLabel(QStringLiteral("仅保留最近 400 条日志；SYS3 不识别位流文件或测量电压。FPGA TX/BIST 不替代 PC 收包与样本校验。"));
    note->setWordWrap(true); layout->addWidget(note);
    connect(m_scan, &QPushButton::clicked, this, &Sys3Panel::refreshPorts);
    connect(m_connect, &QPushButton::clicked, this, [this] {
        if (m_client.running()) m_client.close(); else m_client.open(port()); refresh();
    });
    connect(saveLog, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("导出最近串口日志"), "m128-sys3.log", "Log (*.log)");
        if (path.isEmpty()) return;
        QSaveFile file(path); const QByteArray bytes = (m_client.snapshot().log.join('\n') + '\n').toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            QMessageBox::warning(this, QStringLiteral("日志导出失败"), file.errorString());
    });
    auto *timer = new QTimer(this); timer->setInterval(100); connect(timer, &QTimer::timeout, this, &Sys3Panel::refresh); timer->start(); refresh();
}

void Sys3Panel::refreshPorts()
{
    const QString selected = port(); m_ports->clear(); m_ports->addItems(Sys3Client::availablePorts());
    m_ports->setEditText(selected.isEmpty() ? QStringLiteral("COM3") : selected);
}
QString Sys3Panel::port() const { return m_ports->currentText().trimmed().toUpper(); }
void Sys3Panel::openPort(const QString &port) { m_ports->setEditText(port); m_client.open(port); refresh(); }

QString Sys3Panel::summary() const
{
    const auto s = m_client.snapshot();
    if (!s.error.isEmpty()) return QStringLiteral("SYS3：") + s.error;
    if (!s.connected) return QStringLiteral("SYS3：未连接，硬件遥测未知");
    if (!s.current(m_client.now())) return QStringLiteral("SYS3：等待识别 / 超时，不能作为当前硬件状态");
    return QStringLiteral("SYS3：%1 · 时钟%2\n音频 / 以太网 / RX 域：%3 / %4 / %5")
        .arg(s.port, s.latest.clocksHealthy() ? QStringLiteral("正常") : QStringLiteral("复位或异常"),
            s.latest.audioFresh() ? QStringLiteral("新鲜") : QStringLiteral("过期"), s.latest.ethFresh() ? QStringLiteral("新鲜") : QStringLiteral("过期"), s.latest.rxFresh() ? QStringLiteral("新鲜") : QStringLiteral("过期"));
}

void Sys3Panel::sendCommand(char command)
{
    if (m_captureBusy && (command == 'r' || command == 't')) return;
    if (!m_client.snapshot().commandBlock(command, m_client.now()).isEmpty()) return;
    if (command == 'r' || command == 't' || command == 'c') {
        const QString message = command == 'r'
            ? QStringLiteral("复位 PLL 和完整采集系统，并回到真实麦克风模式。主机将暂停 UDP 接收以避免混合复位前后会话；恢复后请手动开始接收。继续？")
            : command == 't' ? QStringLiteral("切换内部 PDM 自测/真实输入，中止当前采集会话并重新加载校准。UDP 保持接收，新 epoch 将独立审计。继续？")
            : QStringLiteral("仅清除时钟历史故障，不清除当前采集错误或 PHY/MAC 计数。历史取证信息会改变，继续？");
        if (QMessageBox::question(this, QStringLiteral("确认设备操作"), message, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    }
    if ((m_captureBusy && (command == 'r' || command == 't')) || !m_client.snapshot().commandBlock(command, m_client.now()).isEmpty()) { refresh(); return; }
    if (command == 'r') emit aboutToReset();
    m_client.send(command); refresh();
}

void Sys3Panel::refresh()
{
    const auto s = m_client.snapshot(); const auto now = m_client.now(); const bool current = s.current(now);
    m_ports->setEnabled(!m_client.running()); m_scan->setEnabled(!m_client.running());
    m_connect->setText(m_client.running() ? QStringLiteral("断开串口") : QStringLiteral("连接串口"));
    m_status->setText(QStringLiteral("%1 | 有效 %2 · 异常行 %3 · 缺失 %4 · 重复 %5 · 观察域重启 %6")
        .arg(summary()).arg(s.valid).arg(s.invalid).arg(s.missing).arg(s.duplicates).arg(s.restarts));
    const bool fresh[] {current, current && s.latest.audioFresh(), current && s.latest.ethFresh(), current && s.latest.rxFresh()};
    m_freshness->setText(s.lastFrameMs < 0 ? QStringLiteral("等待严格的 312 字节 SYS3 帧；CLK2 不兼容，连接不会自动发送命令。")
        : QStringLiteral("最近有效帧 %1 ms 前 · 超时界限 1500 ms；域新鲜位为 0 时，下表是历史快照。独立观察计时不是 UTC。").arg(now - s.lastFrameMs));
    for (int i = 0; i < 5; ++i) {
        const char command = "shrtc"[i]; QString reason = s.commandBlock(command, now);
        if (m_captureBusy && (command == 'r' || command == 't')) reason = QStringLiteral("请先停止录制并等待写盘完成");
        m_commands[i]->setEnabled(reason.isEmpty()); m_commands[i]->setToolTip(reason.isEmpty() ? QStringLiteral("发送单字符，不添加换行；回执和动作完成分别确认，不自动重试。") : reason);
    }
    m_commandStatus->setText(s.commandStatus);
    if (!isVisible()) return;
    const auto rows = sys3Details(s.latest); m_details->setRowCount(rows.size());
    const QStringList domains {QStringLiteral("观察域"), QStringLiteral("音频域"), QStringLiteral("以太网域"), QStringLiteral("RX 域")};
    auto age = [&](int d) { return !s.valid ? QStringLiteral("未知") : (fresh[d] ? QStringLiteral("新鲜 / ") : QStringLiteral("历史 / ")) + domains[d]; };
    for (int i = 0; i < rows.size(); ++i) {
        cell(m_details, i, 0, rows[i][0]); cell(m_details, i, 1, s.valid ? rows[i][1] : QStringLiteral("—")); cell(m_details, i, 2, age(rows[i][2].toInt()));
    }
    const auto names = sys3WordNames();
    for (int i = 0; i < 34; ++i) {
        cell(m_words, i, 0, QString::number(i)); cell(m_words, i, 1, names[i]);
        cell(m_words, i, 2, s.valid ? QStringLiteral("%1").arg(s.latest.words[i], 8, 16, QChar('0')).toUpper() : QStringLiteral("—"));
        cell(m_words, i, 3, s.valid ? QString::number(s.latest.words[i]) : QStringLiteral("—")); cell(m_words, i, 4, age(sys3WordDomain(i)));
    }
    if (m_seenLog != s.log) {
        const int scroll = m_log->verticalScrollBar()->value(); const bool atEnd = scroll == m_log->verticalScrollBar()->maximum();
        m_seenLog = s.log; m_log->setPlainText(s.log.join('\n'));
        m_log->verticalScrollBar()->setValue(atEnd ? m_log->verticalScrollBar()->maximum() : scroll);
    }
}

} // namespace MicArray::Array128
