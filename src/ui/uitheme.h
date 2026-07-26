#pragma once

#include <QIcon>
#include <QPalette>
#include <QString>

namespace MicArray {

enum class UiTheme {
    Light,
    Dark,
};

UiTheme uiThemeFromName(const QString &name);
QString uiThemeName(UiTheme theme);
QPalette paletteForUiTheme(UiTheme theme);
QString styleSheetForUiTheme(UiTheme theme);
QIcon bulbIconForUiTheme(UiTheme theme);

} // namespace MicArray
