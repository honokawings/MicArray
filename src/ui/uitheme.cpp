#include "ui/uitheme.h"

#include <QColor>
#include <QPainter>
#include <QPixmap>

namespace MicArray {

UiTheme uiThemeFromName(const QString &name)
{
    return name.trimmed().compare(QStringLiteral("dark"), Qt::CaseInsensitive) == 0
        ? UiTheme::Dark
        : UiTheme::Light;
}

QString uiThemeName(UiTheme theme)
{
    return theme == UiTheme::Dark ? QStringLiteral("dark") : QStringLiteral("light");
}

QPalette paletteForUiTheme(UiTheme theme)
{
    const bool dark = theme == UiTheme::Dark;
    const QColor window = dark ? QColor(QStringLiteral("#151a21")) : QColor(QStringLiteral("#f3f6fa"));
    const QColor windowText = dark ? QColor(QStringLiteral("#e8eef5")) : QColor(QStringLiteral("#1d2a36"));
    const QColor base = dark ? QColor(QStringLiteral("#0f141a")) : QColor(QStringLiteral("#ffffff"));
    const QColor alternateBase = dark ? QColor(QStringLiteral("#1c242e")) : QColor(QStringLiteral("#edf2f7"));
    const QColor button = dark ? QColor(QStringLiteral("#252e3a")) : QColor(QStringLiteral("#ffffff"));
    const QColor border = dark ? QColor(QStringLiteral("#3b4858")) : QColor(QStringLiteral("#b9c5d1"));
    const QColor highlight = dark ? QColor(QStringLiteral("#3b92e8")) : QColor(QStringLiteral("#1677c8"));

    QPalette palette;
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, windowText);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, alternateBase);
    palette.setColor(QPalette::ToolTipBase, dark ? QColor(QStringLiteral("#27313e"))
                                                 : QColor(QStringLiteral("#fff7d6")));
    palette.setColor(QPalette::ToolTipText, windowText);
    palette.setColor(QPalette::Text, windowText);
    palette.setColor(QPalette::Button, button);
    palette.setColor(QPalette::ButtonText, windowText);
    palette.setColor(QPalette::BrightText, QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::Highlight, highlight);
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::Mid, border);
    palette.setColor(QPalette::Shadow, dark ? QColor(QStringLiteral("#070a0d"))
                                            : QColor(QStringLiteral("#7d8995")));
    palette.setColor(QPalette::Link, dark ? QColor(QStringLiteral("#69b7ff"))
                                          : QColor(QStringLiteral("#0969a9")));
    palette.setColor(QPalette::Disabled, QPalette::WindowText,
                     dark ? QColor(QStringLiteral("#778392")) : QColor(QStringLiteral("#7f8b96")));
    palette.setColor(QPalette::Disabled, QPalette::Text,
                     dark ? QColor(QStringLiteral("#778392")) : QColor(QStringLiteral("#7f8b96")));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText,
                     dark ? QColor(QStringLiteral("#778392")) : QColor(QStringLiteral("#7f8b96")));
    palette.setColor(QPalette::Disabled, QPalette::Button,
                     dark ? QColor(QStringLiteral("#202730")) : QColor(QStringLiteral("#e5e9ee")));
    return palette;
}

QString styleSheetForUiTheme(UiTheme theme)
{
    const bool dark = theme == UiTheme::Dark;
    const QString border = dark ? QStringLiteral("#3b4858") : QStringLiteral("#b9c5d1");
    const QString panel = dark ? QStringLiteral("#1b222c") : QStringLiteral("#ffffff");
    const QString control = dark ? QStringLiteral("#252e3a") : QStringLiteral("#ffffff");
    const QString hover = dark ? QStringLiteral("#303c4b") : QStringLiteral("#e7f1fb");
    const QString pressed = dark ? QStringLiteral("#17202a") : QStringLiteral("#d5e7f8");
    const QString accent = dark ? QStringLiteral("#52a8ff") : QStringLiteral("#1677c8");
    const QString text = dark ? QStringLiteral("#e8eef5") : QStringLiteral("#1d2a36");
    const QString disabledText = dark ? QStringLiteral("#778392") : QStringLiteral("#7f8b96");
    const QString disabledButton = dark ? QStringLiteral("#202730") : QStringLiteral("#e5e9ee");

    QString styleSheet = QStringLiteral(R"(
QMainWindow {
    background: %1;
}
QGroupBox {
    background: %2;
    border: 1px solid %3;
    border-radius: 8px;
    margin-top: 10px;
    padding-top: 8px;
    font-weight: 600;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0 4px;
    color: %4;
}
QTabWidget::pane {
    border: 1px solid %3;
    border-radius: 7px;
    background: %2;
}
QTabBar::tab {
    background: %5;
    color: %4;
    border: 1px solid %3;
    border-bottom: none;
    padding: 7px 13px;
    margin-right: 2px;
    border-top-left-radius: 6px;
    border-top-right-radius: 6px;
}
QTabBar::tab:selected {
    background: %2;
    color: %6;
}
QPushButton, QComboBox, QSpinBox, QDoubleSpinBox, QLineEdit {
    background: %5;
    color: %4;
    border: 1px solid %3;
    border-radius: 6px;
    padding: 5px 8px;
}
QPushButton:hover, QComboBox:hover, QSpinBox:hover,
QDoubleSpinBox:hover, QLineEdit:hover {
    background: %7;
    border-color: %6;
}
QPushButton:pressed {
    background: %8;
}
QPushButton:disabled, QComboBox:disabled,
QSpinBox:disabled, QDoubleSpinBox:disabled, QLineEdit:disabled {
    color: @disabledText@;
    background: @disabledButton@;
}
QPlainTextEdit, QTextEdit, QTableView, QListView {
    background: palette(base);
    color: %4;
    border: 1px solid %3;
    border-radius: 6px;
    selection-background-color: %6;
}
QHeaderView::section {
    background: %5;
    color: %4;
    border: none;
    border-right: 1px solid %3;
    border-bottom: 1px solid %3;
    padding: 5px;
}
QProgressBar {
    border: 1px solid %3;
    border-radius: 5px;
    background: palette(base);
    text-align: center;
}
QProgressBar::chunk {
    background: %6;
    border-radius: 4px;
}
QScrollBar:vertical, QScrollBar:horizontal {
    background: transparent;
    border: none;
    margin: 2px;
}
QScrollBar::handle {
    background: %3;
    border-radius: 4px;
    min-height: 24px;
    min-width: 24px;
}
QScrollBar::add-line, QScrollBar::sub-line {
    width: 0;
    height: 0;
}
QToolTip {
    color: %4;
    background: %2;
    border: 1px solid %3;
    padding: 5px;
}
QToolButton#themeToggleButton {
    background: %5;
    color: %4;
    border: 1px solid %3;
    border-radius: 15px;
    padding: 3px;
}
QToolButton#themeToggleButton:hover {
    background: %7;
    border-color: %6;
}
QToolButton#themeToggleButton:pressed {
    background: %8;
}
QToolButton:checked {
    background: %6;
    color: white;
    border: 1px solid %6;
}
QToolButton[recordingChannelButton="true"] {
    background: %5;
    color: %4;
    border: 1px solid %3;
    border-radius: 6px;
    padding: 5px;
}
QToolButton[recordingChannelButton="true"]:hover {
    background: %7;
    color: %4;
    border-color: %6;
}
QToolButton[recordingChannelButton="true"]:checked {
    background: %6;
    color: #ffffff;
    border-color: %6;
}
QToolButton[recordingChannelButton="true"]:disabled {
    background: @disabledButton@;
    color: @disabledText@;
    border-color: %3;
}
)")
        .arg(dark ? QStringLiteral("#151a21") : QStringLiteral("#f3f6fa"),
             panel,
             border,
             text,
             control,
             accent,
             hover,
             pressed);
    styleSheet.replace(QStringLiteral("@disabledText@"), disabledText);
    styleSheet.replace(QStringLiteral("@disabledButton@"), disabledButton);
    return styleSheet;
}

QIcon bulbIconForUiTheme(UiTheme theme)
{
    constexpr int logicalSize = 24;
    const bool light = theme == UiTheme::Light;
    const QColor stroke = light ? QColor(QStringLiteral("#b56a00"))
                                : QColor(QStringLiteral("#d5dde7"));
    const QColor glow = QColor(QStringLiteral("#ffd54f"));
    const auto render = [light, stroke, glow](qreal scale) {
        QPixmap pixmap(static_cast<int>(logicalSize * scale),
                       static_cast<int>(logicalSize * scale));
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(stroke, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(light ? QBrush(glow) : Qt::NoBrush);
        painter.drawEllipse(QRectF(6.3, 3.4, 11.4, 11.4));
        painter.drawLine(QPointF(8.7, 13.0), QPointF(10.0, 16.0));
        painter.drawLine(QPointF(15.3, 13.0), QPointF(14.0, 16.0));
        painter.drawLine(QPointF(10.0, 16.0), QPointF(14.0, 16.0));
        painter.drawLine(QPointF(9.8, 18.2), QPointF(14.2, 18.2));
        painter.drawLine(QPointF(10.7, 20.2), QPointF(13.3, 20.2));
        if (light) {
            painter.drawLine(QPointF(12.0, 0.8), QPointF(12.0, 2.2));
            painter.drawLine(QPointF(3.8, 4.0), QPointF(5.0, 5.2));
            painter.drawLine(QPointF(20.2, 4.0), QPointF(19.0, 5.2));
            painter.drawLine(QPointF(2.0, 10.4), QPointF(4.0, 10.4));
            painter.drawLine(QPointF(20.0, 10.4), QPointF(22.0, 10.4));
        }
        return pixmap;
    };

    QIcon icon;
    icon.addPixmap(render(2.0));
    icon.addPixmap(render(4.0));
    return icon;
}

} // namespace MicArray
