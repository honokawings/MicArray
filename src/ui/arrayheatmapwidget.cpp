#include "ui/arrayheatmapwidget.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace MicArray {

ArrayHeatmapWidget::ArrayHeatmapWidget(QWidget *parent)
    : QWidget(parent)
    , m_amplitudes(48, 0.0)
    , m_shifts(48, 0)
    , m_automatic(48, false)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto *header = new QWidget(this);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    auto *title = new QLabel(QStringLiteral("48 通道阵列幅值与移位增益"), header);
    title->setObjectName(QStringLiteral("arrayHeatmapTitle"));
    title->setStyleSheet(QStringLiteral("font-weight:600;"));
    headerLayout->addWidget(title);
    headerLayout->addStretch();
    auto createCommand = [header, headerLayout](const QString &text, const QString &name) {
        auto *button = new QToolButton(header);
        button->setText(text);
        button->setObjectName(name);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        headerLayout->addWidget(button);
        return button;
    };
    QToolButton *allAutomatic = createCommand(QStringLiteral("全部自动"), QStringLiteral("gainAllAutomaticButton"));
    QToolButton *allManual = createCommand(QStringLiteral("全部手动"), QStringLiteral("gainAllManualButton"));
    QToolButton *reset = createCommand(QStringLiteral("复位"), QStringLiteral("gainResetButton"));
    QToolButton *save = createCommand(QStringLiteral("保存"), QStringLiteral("gainSaveButton"));
    QToolButton *load = createCommand(QStringLiteral("载入"), QStringLiteral("gainLoadButton"));
    layout->addWidget(header);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("gainArrayScrollArea"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    auto *content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("gainArrayContent"));
    content->setMinimumSize(700, 700);

    auto *grid = new QGridLayout(content);
    grid->setContentsMargins(2, 2, 2, 2);
    grid->setSpacing(4);
    for (int channel = 0; channel < 48; ++channel) {
        auto *channelWidget = new QWidget(content);
        channelWidget->setObjectName(QStringLiteral("channelGainPanel_%1").arg(channel));
        channelWidget->setFixedSize(82, 112);
        auto *channelLayout = new QHBoxLayout(channelWidget);
        channelLayout->setContentsMargins(2, 2, 2, 2);
        channelLayout->setSpacing(2);
        auto *cell = new QToolButton(channelWidget);
        cell->setObjectName(QStringLiteral("channelCell_%1").arg(channel));
        cell->setFixedSize(56, 104);
        cell->setToolButtonStyle(Qt::ToolButtonTextOnly);
        connect(cell, &QToolButton::clicked, this, [this, channel]() {
            setSelectedChannel(channel);
            emit channelSelected(channel);
        });
        auto *gainColumn = new QWidget(channelWidget);
        auto *gainLayout = new QVBoxLayout(gainColumn);
        gainLayout->setContentsMargins(0, 0, 0, 0);
        gainLayout->setSpacing(0);
        auto *slider = new QSlider(Qt::Vertical, gainColumn);
        slider->setObjectName(QStringLiteral("channelGainSlider_%1").arg(channel));
        slider->setRange(GainConfiguration::MinimumShift, GainConfiguration::MaximumShift);
        slider->setValue(0);
        slider->setTickPosition(QSlider::TicksBothSides);
        slider->setToolTip(QStringLiteral("CH%1 移位增益（bit）").arg(channel));
        auto *automatic = new QCheckBox(QStringLiteral("A"), gainColumn);
        automatic->setObjectName(QStringLiteral("channelAutoGain_%1").arg(channel));
        automatic->setToolTip(QStringLiteral("CH%1 独立自动增益").arg(channel));
        gainLayout->addWidget(slider, 1, Qt::AlignHCenter);
        gainLayout->addWidget(automatic, 0, Qt::AlignCenter);
        channelLayout->addWidget(cell);
        channelLayout->addWidget(gainColumn);
        connect(slider, &QSlider::valueChanged, this, [this, channel](int shift) {
            if (m_automatic.at(channel)) {
                m_automatic[channel] = false;
                const QSignalBlocker blocker(m_automaticChecks.at(channel));
                m_automaticChecks.at(channel)->setChecked(false);
                emit channelAutomaticChanged(channel, false);
            }
            m_shifts[channel] = shift;
            refreshCells();
            emit channelShiftChanged(channel, shift);
        });
        connect(automatic, &QCheckBox::toggled, this, [this, channel](bool enabled) {
            m_automatic[channel] = enabled;
            refreshCells();
            emit channelAutomaticChanged(channel, enabled);
        });
        grid->addWidget(channelWidget, channel / 8, channel % 8);
        m_cells.append(cell);
        m_gainSliders.append(slider);
        m_automaticChecks.append(automatic);
    }
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);

    connect(allAutomatic, &QToolButton::clicked, this, [this]() {
        setAllAutomaticUi(true);
        emit setAllAutomaticRequested(true);
    });
    connect(allManual, &QToolButton::clicked, this, [this]() {
        setAllAutomaticUi(false);
        emit setAllAutomaticRequested(false);
    });
    connect(reset, &QToolButton::clicked, this, [this]() {
        resetGainUi();
        emit resetRequested();
    });
    connect(save, &QToolButton::clicked, this, &ArrayHeatmapWidget::saveRequested);
    connect(load, &QToolButton::clicked, this, &ArrayHeatmapWidget::loadRequested);
    refreshCells();
}

void ArrayHeatmapWidget::setAmplitudes(const QVector<double> &normalizedAmplitudes, int selectedChannel)
{
    if (normalizedAmplitudes.size() != m_cells.size()) {
        return;
    }
    m_amplitudes = normalizedAmplitudes;
    m_selectedChannel = std::clamp(selectedChannel, 0, static_cast<int>(m_cells.size()) - 1);
    m_peakChannel = static_cast<int>(std::distance(m_amplitudes.cbegin(), std::max_element(m_amplitudes.cbegin(), m_amplitudes.cend())));
    refreshCells();
}

void ArrayHeatmapWidget::setSelectedChannel(int channel)
{
    if (channel < 0 || channel >= m_cells.size()) {
        return;
    }
    m_selectedChannel = channel;
    refreshCells();
}

void ArrayHeatmapWidget::setGainConfiguration(const GainConfiguration &configuration)
{
    if (!GainConfiguration::validate(configuration)) {
        return;
    }
    m_shifts = configuration.shifts;
    m_automatic = configuration.automatic;
    for (int channel = 0; channel < m_cells.size(); ++channel) {
        const QSignalBlocker sliderBlocker(m_gainSliders.at(channel));
        const QSignalBlocker automaticBlocker(m_automaticChecks.at(channel));
        m_gainSliders.at(channel)->setValue(m_shifts.at(channel));
        m_automaticChecks.at(channel)->setChecked(m_automatic.at(channel));
    }
    refreshCells();
}

void ArrayHeatmapWidget::setGainSnapshot(const GainSnapshot &snapshot)
{
    if (snapshot.shifts.size() != m_cells.size()
        || snapshot.automatic.size() != m_cells.size()) {
        return;
    }
    GainConfiguration configuration;
    configuration.shifts = snapshot.shifts;
    configuration.automatic = snapshot.automatic;
    setGainConfiguration(configuration);
}

void ArrayHeatmapWidget::refreshCells()
{
    for (int channel = 0; channel < m_cells.size(); ++channel) {
        QToolButton *cell = m_cells.at(channel);
        const double amplitude = std::clamp(m_amplitudes.value(channel), 0.0, 1.0);
        const bool selected = channel == m_selectedChannel;
        const bool peak = channel == m_peakChannel;
        cell->setProperty("selected", selected);
        cell->setProperty("peak", peak);
        const QString shift = m_shifts.at(channel) >= 0
            ? QStringLiteral("+%1").arg(m_shifts.at(channel))
            : QString::number(m_shifts.at(channel));
        cell->setText(QStringLiteral("CH%1\n%2%\n%3 bit %4")
                          .arg(channel, 2, 10, QLatin1Char('0'))
                          .arg(amplitude * 100.0, 0, 'f', 0)
                          .arg(shift)
                          .arg(m_automatic.at(channel) ? QStringLiteral("A") : QStringLiteral("M")));
        const QString border = selected ? QStringLiteral("3px solid #43d9ff")
                                        : peak ? QStringLiteral("3px solid #ffd166")
                                               : QStringLiteral("1px solid #34495e");
        const QString style = QStringLiteral("QToolButton { color:white; border:%1; border-radius:4px; font-weight:600; }")
                                  .arg(border);
        if (cell->styleSheet() != style) {
            cell->setStyleSheet(style);
        }
        QPalette palette = cell->palette();
        const QColor background = colorForAmplitude(amplitude);
        if (palette.color(QPalette::Button) != background) {
            palette.setColor(QPalette::Button, background);
            palette.setColor(QPalette::ButtonText, Qt::white);
            cell->setPalette(palette);
            cell->setAutoFillBackground(true);
        }
    }
}

void ArrayHeatmapWidget::setAllAutomaticUi(bool automatic)
{
    m_automatic.fill(automatic);
    for (QCheckBox *check : m_automaticChecks) {
        const QSignalBlocker blocker(check);
        check->setChecked(automatic);
    }
    refreshCells();
}

void ArrayHeatmapWidget::resetGainUi()
{
    m_shifts.fill(0);
    m_automatic.fill(false);
    for (int channel = 0; channel < m_cells.size(); ++channel) {
        const QSignalBlocker sliderBlocker(m_gainSliders.at(channel));
        const QSignalBlocker automaticBlocker(m_automaticChecks.at(channel));
        m_gainSliders.at(channel)->setValue(0);
        m_automaticChecks.at(channel)->setChecked(false);
    }
    refreshCells();
}

QColor ArrayHeatmapWidget::colorForAmplitude(double value)
{
    const double clamped = std::clamp(value, 0.0, 1.0);
    const int red = static_cast<int>(35 + 210 * clamped);
    const int green = static_cast<int>(70 + 90 * (1.0 - std::abs(clamped - 0.5) * 2.0));
    const int blue = static_cast<int>(170 * (1.0 - clamped));
    return QColor(red, green, blue);
}

} // namespace MicArray
