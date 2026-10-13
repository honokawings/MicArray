#include "sys3panel.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>

namespace MicArray::Array128 {
namespace {
class PortCombo final : public QComboBox
{
protected:
    void wheelEvent(QWheelEvent *event) override { event->ignore(); }
};
class AudioChannelSpin final : public QSpinBox
{
protected:
    void wheelEvent(QWheelEvent *event) override { event->ignore(); }
};
class AudioGainSpin final : public QDoubleSpinBox
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
    m_status = new QLabel; m_status->setObjectName("sys3Status"); m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); layout->addWidget(m_status);
    auto *tabs = new QTabWidget; tabs->setObjectName("sys3DetailsTabs"); layout->addWidget(tabs, 1);
    auto *systemPage = new QWidget; auto *systemLayout = new QVBoxLayout(systemPage);
    auto *commands = new QHBoxLayout;
    const QStringList names {QStringLiteral("读取状态 (s)"), QStringLiteral("状态别名 (h)"), QStringLiteral("系统复位 (r)"), QStringLiteral("切换自测 (t)"), QStringLiteral("清时钟历史 (c)")};
    for (int i = 0; i < 5; ++i) {
        const char command = "shrtc"[i]; auto *button = new QPushButton(names[i]); m_commands[i] = button;
        button->setObjectName(QString("sys3Command_%1").arg(QChar(command))); commands->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, command] { sendCommand(command); });
    }
    commands->addStretch(); systemLayout->addLayout(commands);
    m_commandStatus = new QLabel; m_commandStatus->setWordWrap(true); systemLayout->addWidget(m_commandStatus);
    m_details = table({QStringLiteral("参数"), QStringLiteral("设备快照值"), QStringLiteral("时效 / 所属域")});
    m_details->setObjectName("sys3DecodedTable"); systemLayout->addWidget(m_details, 1); tabs->addTab(systemPage, QStringLiteral("系统状态"));
    tabs->addTab(buildAudioPanel(), QStringLiteral("音频控制"));
    m_words = table({QStringLiteral("索引"), QStringLiteral("字段"), QStringLiteral("HEX"), QStringLiteral("无符号值"), QStringLiteral("时效")});
    m_words->setObjectName("sys3RawTable"); m_words->setRowCount(34);
    m_words->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    auto *rawPage = new QWidget; auto *rawLayout = new QVBoxLayout(rawPage);
    m_rawSource = new PortCombo; m_rawSource->setObjectName("uartRawSource");
    m_rawSource->addItems({QStringLiteral("SYS3 · 34 字"), QStringLiteral("I2S1 / GAIN1 · 最近 14 字")});
    rawLayout->addWidget(m_rawSource); rawLayout->addWidget(m_words, 1); tabs->addTab(rawPage, QStringLiteral("原始字"));
    connect(m_rawSource, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    m_log = new QPlainTextEdit; m_log->setReadOnly(true); m_log->setMaximumBlockCount(400); tabs->addTab(m_log, QStringLiteral("事件记录"));
    m_log->setToolTip(QStringLiteral("仅保留最近 400 条日志；SYS3 不识别位流文件或测量电压。FPGA TX/BIST 不替代 PC 收包与样本校验。"));
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

QWidget *Sys3Panel::buildAudioPanel()
{
    auto *page = new QWidget; page->setObjectName("uartAudioPanel"); auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(6, 6, 6, 6); layout->setSpacing(4);
    auto *controls = new QHBoxLayout;
    auto *i2s = new QGroupBox(QStringLiteral("板端 I2S 监听")); auto *i2sLayout = new QGridLayout(i2s);
    i2s->setObjectName("uartAudioGroup"); i2sLayout->setContentsMargins(6, 6, 6, 6); i2sLayout->setSpacing(4);
    m_i2sChannel = new AudioChannelSpin; m_i2sChannel->setRange(0, 127); m_i2sChannel->setPrefix("CH "); m_i2sChannel->setObjectName("uartI2sChannel");
    m_i2sChannel->setToolTip(QStringLiteral("校准后 PCM 通道；与电脑扬声器监听独立。关闭监听保留通道，I2S 时钟继续运行。"));
    i2sLayout->addWidget(new QLabel(QStringLiteral("通道")), 0, 0); i2sLayout->addWidget(m_i2sChannel, 0, 1);
    m_loadAudioRequest = new QPushButton(QStringLiteral("载入最近请求")); m_loadAudioRequest->setObjectName("uartLoadRequested");
    m_loadAudioRequest->setToolTip(QStringLiteral("仅把最近音频回执的请求通道和增益填入编辑框，不发送命令；过期时请先查询。"));
    i2sLayout->addWidget(m_loadAudioRequest, 0, 2);
    connect(m_loadAudioRequest, &QPushButton::clicked, this, [this] {
        const auto s = m_client.snapshot(); if (!s.audioIdentified || s.audioPending) return;
        m_i2sChannel->setValue(int(s.latestAudio.words[2] & 127));
        m_evenGain->setValue(double(s.latestAudio.words[4] & 0xffffff) / 4194304.0);
        m_oddGain->setValue(double(s.latestAudio.words[5] & 0xffffff) / 4194304.0);
    });
    auto *gain = new QGroupBox(QStringLiteral("奇偶增益 · Q2.22")); auto *gainLayout = new QGridLayout(gain);
    gain->setObjectName("uartAudioGroup"); gainLayout->setContentsMargins(6, 6, 6, 6); gainLayout->setSpacing(4);
    m_evenGain = new AudioGainSpin; m_oddGain = new AudioGainSpin;
    m_evenGain->setObjectName("uartEvenGain"); m_oddGain->setObjectName("uartOddGain");
    for (auto *spin : {m_evenGain, m_oddGain}) {
        spin->setDecimals(7); spin->setRange(0, 8388607.0 / 4194304.0); spin->setSingleStep(.05); spin->setSuffix(QStringLiteral(" ×")); spin->setValue(1);
        spin->setToolTip(QStringLiteral("非负 Q2.22，范围 000000–7FFFFF；1× = 400000，0× 会将该组真实音频归零。"));
    }
    gainLayout->addWidget(new QLabel(QStringLiteral("偶数 CH")), 0, 0); gainLayout->addWidget(m_evenGain, 0, 1);
    gainLayout->addWidget(new QLabel(QStringLiteral("奇数 CH")), 0, 2); gainLayout->addWidget(m_oddGain, 0, 3);
    const std::array<AudioUartAction, 5> actions {AudioUartAction::QueryI2s, AudioUartAction::SelectI2s,
        AudioUartAction::DisableI2s, AudioUartAction::QueryGain, AudioUartAction::SetGain};
    const QStringList names {QStringLiteral("查询监听"), QStringLiteral("启用监听"), QStringLiteral("关闭监听"), QStringLiteral("查询增益"), QStringLiteral("应用增益")};
    const QStringList objects {"uartQueryI2s", "uartApplyI2s", "uartDisableI2s", "uartQueryGain", "uartApplyGain"};
    for (int i = 0; i < 5; ++i) {
        auto *button = new QPushButton(names[i]); m_audioCommands[i] = button; button->setObjectName(objects[i]);
        connect(button, &QPushButton::clicked, this, [this, action = actions[i]] { sendAudio(action); });
    }
    auto *i2sButtons = new QHBoxLayout; for (int i = 0; i < 3; ++i) i2sButtons->addWidget(m_audioCommands[i]);
    i2sLayout->addLayout(i2sButtons, 1, 0, 1, 3);
    auto *unity = new QPushButton(QStringLiteral("填入 1×")); m_unityGain = unity; unity->setObjectName("uartUnityPreset");
    unity->setToolTip(QStringLiteral("仅填入参数；点击“应用增益”后发送。"));
    connect(unity, &QPushButton::clicked, this, [this] { m_evenGain->setValue(1); m_oddGain->setValue(1); });
    auto *gainButtons = new QHBoxLayout; gainButtons->addWidget(unity); gainButtons->addWidget(m_audioCommands[3]); gainButtons->addWidget(m_audioCommands[4]);
    gainLayout->addLayout(gainButtons, 1, 0, 1, 4);
    auto encoding = [this] {
        for (auto *spin : {m_evenGain, m_oddGain}) {
            spin->setToolTip(QStringLiteral("编辑值 %1\n非负 Q2.22，范围 000000–7FFFFF；1× = 400000。0× 会将该组真实音频归零；内部 PDM 自测旁路增益。")
                .arg(audioGainText(quint32(std::llround(spin->value() * 4194304)))));
        }
    };
    connect(m_evenGain, &QDoubleSpinBox::valueChanged, this, encoding); connect(m_oddGain, &QDoubleSpinBox::valueChanged, this, encoding); encoding();
    controls->addWidget(i2s, 1); controls->addWidget(gain, 1); layout->addLayout(controls);
    m_audioStatus = new QLabel; m_audioStatus->setObjectName("uartAudioStatus"); m_audioStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); layout->addWidget(m_audioStatus);
    m_audioValues = table({QStringLiteral("参数"), QStringLiteral("设备请求值"), QStringLiteral("音频域实际值")});
    m_audioValues->setObjectName("uartAudioValues"); m_audioValues->setRowCount(3);
    m_audioValues->verticalHeader()->setDefaultSectionSize(26); m_audioValues->horizontalHeader()->setFixedHeight(28);
    m_audioValues->setFixedHeight(108); m_audioValues->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_audioValues->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); layout->addWidget(m_audioValues);
    m_audioCounters = new QLabel; m_audioCounters->setObjectName("uartAudioCounters"); layout->addWidget(m_audioCounters);
    m_dcStatus = new QLabel; m_dcStatus->setObjectName("uartDcStatus"); layout->addWidget(m_dcStatus);
    layout->addStretch();
    return page;
}

void Sys3Panel::refreshPorts()
{
    const QString selected = port(); m_ports->clear(); m_ports->addItems(Sys3Client::availablePorts());
    m_ports->setEditText(selected.isEmpty() ? QStringLiteral("COM3") : selected);
}
QString Sys3Panel::port() const { return m_ports->currentText().trimmed().toUpper(); }
void Sys3Panel::openPort(const QString &port) { m_ports->setEditText(port); m_client.open(port); refresh(); }
bool Sys3Panel::acquisitionChangePending() const
{
    const auto s = m_client.snapshot();
    return s.pending == 'r' || s.pending == 't' || (s.audioPending && s.audioCommand.action == AudioUartAction::SetGain);
}

QString Sys3Panel::summary() const
{
    const auto s = m_client.snapshot();
    if (!s.error.isEmpty()) return QStringLiteral("SYS3：") + s.error;
    if (!s.connected) return QStringLiteral("SYS3：未连接，硬件遥测未知");
    if (s.receiveBacklog) return QStringLiteral("SYS3：接收积压，正在排空并重新同步；命令已锁定");
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

void Sys3Panel::sendAudio(AudioUartAction action)
{
    AudioUartCommand command; command.action = action; command.channel = m_i2sChannel->value();
    command.evenGain = quint32(std::llround(m_evenGain->value() * 4194304)); command.oddGain = quint32(std::llround(m_oddGain->value() * 4194304));
    m_client.sendAudio(command); refresh();
}

void Sys3Panel::refresh()
{
    const auto s = m_client.snapshot(); const auto now = m_client.now(); const bool current = s.current(now);
    m_ports->setEnabled(!m_client.running()); m_scan->setEnabled(!m_client.running());
    m_connect->setText(m_client.running() ? QStringLiteral("断开串口") : QStringLiteral("连接串口"));
    const auto state = !s.error.isEmpty() ? s.error : !s.connected ? QStringLiteral("未连接 · 硬件状态未知")
        : s.receiveBacklog ? QStringLiteral("接收积压 · 命令已锁定") : !current ? QStringLiteral("等待 SYS3 / 状态过期")
        : s.latest.clocksHealthy() ? QStringLiteral("已连接 · 时钟正常") : QStringLiteral("已连接 · 时钟复位或异常");
    m_status->setText(QStringLiteral("%1 · %2 | SYS3 %3 · 音频回帧 %4 · 异常 %5")
        .arg(s.connected ? s.port : port(), state).arg(s.valid).arg(s.audioValid).arg(s.invalid));
    const bool fresh[] {current, current && s.latest.audioFresh(), current && s.latest.ethFresh(), current && s.latest.rxFresh()};
    const auto freshness = s.lastFrameMs < 0 ? QStringLiteral("等待严格的 312 字节 SYS3 帧；CLK2 不兼容，连接不会自动发送命令。")
        : QStringLiteral("SYS3 %1 ms 前 · 音频回帧 %2 · 域新鲜与主机时效共同判定；过期实际值显示“—”。")
            .arg(now - s.lastFrameMs).arg(s.lastAudioFrameMs < 0 ? QStringLiteral("未查询") : QStringLiteral("%1 ms 前").arg(now - s.lastAudioFrameMs));
    m_status->setToolTip(summary() + '\n' + freshness + QStringLiteral("\nSYS3 缺失 %1 · 音频缺失 %2 · 接收中断 %3\n连接不会自动发送命令；SYS3 不识别位流身份。")
        .arg(s.missing).arg(s.audioMissing).arg(s.receiveDiscontinuities));
    for (int i = 0; i < 5; ++i) {
        const char command = "shrtc"[i]; QString reason = s.commandBlock(command, now);
        if (m_captureBusy && (command == 'r' || command == 't')) reason = QStringLiteral("请先停止录制并等待写盘完成");
        m_commands[i]->setEnabled(reason.isEmpty()); m_commands[i]->setToolTip(reason.isEmpty() ? QStringLiteral("发送单字符，不添加换行；回执和动作完成分别确认，不自动重试。") : reason);
    }
    m_commandStatus->setText(s.commandStatus);
    const bool audioCurrent = s.audioCurrent(now), actual = audioCurrent && s.latestAudio.actualValid();
    const std::array<AudioUartAction, 5> actions {AudioUartAction::QueryI2s, AudioUartAction::SelectI2s,
        AudioUartAction::DisableI2s, AudioUartAction::QueryGain, AudioUartAction::SetGain};
    for (int i = 0; i < 5; ++i) {
        AudioUartCommand command; command.action = actions[i]; command.channel = m_i2sChannel->value();
        command.evenGain = quint32(std::llround(m_evenGain->value() * 4194304)); command.oddGain = quint32(std::llround(m_oddGain->value() * 4194304));
        const auto reason = s.audioCommandBlock(command, now);
        m_audioCommands[i]->setEnabled(reason.isEmpty());
        m_audioCommands[i]->setToolTip(reason.isEmpty() ? QStringLiteral("一次发送一条命令；设置后按回应逐次查询确认，不重复发送设置。") : reason);
    }
    m_i2sChannel->setEnabled(!s.audioPending); m_evenGain->setEnabled(!s.audioPending); m_oddGain->setEnabled(!s.audioPending);
    m_unityGain->setEnabled(!s.audioPending);
    m_loadAudioRequest->setEnabled(s.audioIdentified && !s.audioPending);
    const auto deviceStatus = s.audioValid ? QStringLiteral("%1 · %2 · %3 · 回应队列溢出 %4")
        .arg(!audioCurrent ? QStringLiteral("回帧已过期") : !s.latestAudio.actualValid() ? QStringLiteral("音频域未就绪")
            : s.latestAudio.pending() ? QStringLiteral("配置待生效") : QStringLiteral("当前配置已生效"), s.latestAudio.errorDescription(),
            s.latestAudio.resetReleased() ? QStringLiteral("音频复位释放") : QStringLiteral("音频复位中")).arg(s.latestAudio.queueOverflows())
        : QStringLiteral("需要新版 I2S1 / GAIN1 回帧；请先查询。板端监听与电脑监听独立。");
    m_audioStatus->setText(s.audioCommandStatus);
    m_audioStatus->setToolTip(s.audioCommandStatus + '\n' + deviceStatus + '\n' + freshness);
    m_audioValues->setToolTip(deviceStatus + QStringLiteral("\n设备请求值来自回执；当前音频域无有效快照或主机时效过期时，实际值显示未知。"));
    auto i2sValue = [](quint32 value) { return QStringLiteral("CH%1 · %2").arg(value & 127).arg(value & 128 ? QStringLiteral("启用") : QStringLiteral("静音")); };
    const QStringList requested {i2sValue(s.latestAudio.words[2]), audioGainText(s.latestAudio.words[4]), audioGainText(s.latestAudio.words[5])};
    const QStringList applied {i2sValue(s.latestAudio.words[3]), audioGainText(s.latestAudio.words[6]), audioGainText(s.latestAudio.words[7])};
    const QStringList parameter {QStringLiteral("I2S 监听"), QStringLiteral("偶数通道增益"), QStringLiteral("奇数通道增益")};
    for (int i = 0; i < 3; ++i) {
        cell(m_audioValues, i, 0, parameter[i]); cell(m_audioValues, i, 1, s.audioValid ? requested[i] + (audioCurrent ? QString() : QStringLiteral("（历史）")) : QStringLiteral("—"));
        cell(m_audioValues, i, 2, actual ? applied[i] : QStringLiteral("—（当前值未知）"));
    }
    m_audioCounters->setText(actual ? QStringLiteral("PCM %1 · I2S 帧 %2 · 欠载 %3 · 串行验证 %4 · 串行错误 %5 · 最近 PCM %6")
        .arg(s.latestAudio.words[8]).arg(s.latestAudio.words[9]).arg(s.latestAudio.words[10]).arg(s.latestAudio.words[12]).arg(s.latestAudio.words[13]).arg(s.latestAudio.lastPcm())
        : QStringLiteral("音频计数未知：等待新鲜音频快照。复位释放位不能单独证明时钟或数据正常。"));
    const auto audioFlags = s.latest.words[8];
    m_dcStatus->setText(!fresh[1] ? QStringLiteral("直流补偿：等待新鲜 SYS3 音频域状态。")
        : s.latest.selftest() ? QStringLiteral("内部 PDM 自测：奇偶增益和直流处理旁路；表中实际增益仅为配置寄存器。")
        : !(audioFlags & (1 << 12)) ? QStringLiteral("直流使能位未置位；旧位流不提供该位，需核对版本。")
        : !(audioFlags & (1 << 13)) ? QStringLiteral("直流补偿已启用，正在等待估计审核完成。")
        : audioFlags & (1 << 14) ? QStringLiteral("直流审核结束：存在被拒绝通道，请结合饱和标志和通道质量排查。")
        : QStringLiteral("直流审核结束，拒绝位未置位；仍需检查饱和和实际通道质量。"));
    m_dcStatus->setToolTip(m_dcStatus->text()); m_audioCounters->setToolTip(m_audioCounters->text());
    if (!isVisible()) return;
    const auto rows = sys3Details(s.latest); m_details->setRowCount(rows.size());
    const QStringList domains {QStringLiteral("观察域"), QStringLiteral("音频域"), QStringLiteral("以太网域"), QStringLiteral("RX 域")};
    auto age = [&](int d) { return !s.valid ? QStringLiteral("未知") : (fresh[d] ? QStringLiteral("新鲜 / ") : QStringLiteral("历史 / ")) + domains[d]; };
    for (int i = 0; i < rows.size(); ++i) {
        cell(m_details, i, 0, rows[i][0]); cell(m_details, i, 1, s.valid ? rows[i][1] : QStringLiteral("—")); cell(m_details, i, 2, age(rows[i][2].toInt()));
    }
    const bool audioRaw = m_rawSource->currentIndex() == 1;
    const QStringList audioNames {"Audio sequence", "Fresh/Pending/Reset/Error/Queue", "Requested I2S", "Actual I2S",
        "Requested even gain Q2.22", "Requested odd gain Q2.22", "Actual even gain Q2.22", "Actual odd gain Q2.22",
        "Received PCM samples", "I2S output frames", "Underflow frames", "Last PCM s24", "Verified frames", "Serial errors"};
    const auto names = audioRaw ? audioNames : sys3WordNames(); m_words->setRowCount(names.size());
    for (int i = 0; i < names.size(); ++i) {
        const bool exists = audioRaw ? s.audioValid > 0 : s.valid > 0;
        const auto word = audioRaw ? s.latestAudio.words[i] : s.latest.words[i];
        cell(m_words, i, 0, QString::number(i)); cell(m_words, i, 1, names[i]);
        cell(m_words, i, 2, exists ? QStringLiteral("%1").arg(word, 8, 16, QChar('0')).toUpper() : QStringLiteral("—"));
        cell(m_words, i, 3, exists ? QString::number(word) : QStringLiteral("—"));
        cell(m_words, i, 4, audioRaw ? (!exists ? QStringLiteral("未知") : (actual ? QStringLiteral("新鲜 / ") : QStringLiteral("历史 / "))
            + (s.latestAudio.gainResponse ? "GAIN1" : "I2S1")) : age(sys3WordDomain(i)));
    }
    if (m_seenLog != s.log) {
        const int scroll = m_log->verticalScrollBar()->value(); const bool atEnd = scroll == m_log->verticalScrollBar()->maximum();
        m_seenLog = s.log; m_log->setPlainText(s.log.join('\n'));
        m_log->verticalScrollBar()->setValue(atEnd ? m_log->verticalScrollBar()->maximum() : scroll);
    }
}

} // namespace MicArray::Array128
