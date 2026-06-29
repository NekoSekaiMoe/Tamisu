package me.dabao1955.tamisu.ui.theme

import androidx.compose.ui.graphics.Color

sealed class ThemeColors {
    // Light theme
    abstract val primaryLight: Color
    abstract val onPrimaryLight: Color
    abstract val primaryContainerLight: Color
    abstract val onPrimaryContainerLight: Color
    abstract val secondaryLight: Color
    abstract val onSecondaryLight: Color
    abstract val secondaryContainerLight: Color
    abstract val onSecondaryContainerLight: Color
    abstract val tertiaryLight: Color
    abstract val onTertiaryLight: Color
    abstract val tertiaryContainerLight: Color
    abstract val onTertiaryContainerLight: Color
    abstract val errorLight: Color
    abstract val onErrorLight: Color
    abstract val errorContainerLight: Color
    abstract val onErrorContainerLight: Color
    abstract val backgroundLight: Color
    abstract val onBackgroundLight: Color
    abstract val surfaceLight: Color
    abstract val onSurfaceLight: Color
    abstract val surfaceVariantLight: Color
    abstract val onSurfaceVariantLight: Color
    abstract val outlineLight: Color
    abstract val outlineVariantLight: Color
    abstract val scrimLight: Color
    abstract val inverseSurfaceLight: Color
    abstract val inverseOnSurfaceLight: Color
    abstract val inversePrimaryLight: Color
    abstract val surfaceDimLight: Color
    abstract val surfaceBrightLight: Color
    abstract val surfaceContainerLowestLight: Color
    abstract val surfaceContainerLowLight: Color
    abstract val surfaceContainerLight: Color
    abstract val surfaceContainerHighLight: Color
    abstract val surfaceContainerHighestLight: Color
    // Dark theme
    abstract val primaryDark: Color
    abstract val onPrimaryDark: Color
    abstract val primaryContainerDark: Color
    abstract val onPrimaryContainerDark: Color
    abstract val secondaryDark: Color
    abstract val onSecondaryDark: Color
    abstract val secondaryContainerDark: Color
    abstract val onSecondaryContainerDark: Color
    abstract val tertiaryDark: Color
    abstract val onTertiaryDark: Color
    abstract val tertiaryContainerDark: Color
    abstract val onTertiaryContainerDark: Color
    abstract val errorDark: Color
    abstract val onErrorDark: Color
    abstract val errorContainerDark: Color
    abstract val onErrorContainerDark: Color
    abstract val backgroundDark: Color
    abstract val onBackgroundDark: Color
    abstract val surfaceDark: Color
    abstract val onSurfaceDark: Color
    abstract val surfaceVariantDark: Color
    abstract val onSurfaceVariantDark: Color
    abstract val outlineDark: Color
    abstract val outlineVariantDark: Color
    abstract val scrimDark: Color
    abstract val inverseSurfaceDark: Color
    abstract val inverseOnSurfaceDark: Color
    abstract val inversePrimaryDark: Color
    abstract val surfaceDimDark: Color
    abstract val surfaceBrightDark: Color
    abstract val surfaceContainerLowestDark: Color
    abstract val surfaceContainerLowDark: Color
    abstract val surfaceContainerDark: Color
    abstract val surfaceContainerHighDark: Color
    abstract val surfaceContainerHighestDark: Color

    object Default : ThemeColors() {
        override val primaryLight = Color(0xFF415F91)
        override val onPrimaryLight = Color(0xFFFFFFFF)
        override val primaryContainerLight = Color(0xFFD6E3FF)
        override val onPrimaryContainerLight = Color(0xFF284777)
        override val secondaryLight = Color(0xFF565F71)
        override val onSecondaryLight = Color(0xFFFFFFFF)
        override val secondaryContainerLight = Color(0xFFDAE2F9)
        override val onSecondaryContainerLight = Color(0xFF3E4759)
        override val tertiaryLight = Color(0xFF705575)
        override val onTertiaryLight = Color(0xFFFFFFFF)
        override val tertiaryContainerLight = Color(0xFFFAD8FD)
        override val onTertiaryContainerLight = Color(0xFF573E5C)
        override val errorLight = Color(0xFFBA1A1A)
        override val onErrorLight = Color(0xFFFFFFFF)
        override val errorContainerLight = Color(0xFFFFDAD6)
        override val onErrorContainerLight = Color(0xFF93000A)
        override val backgroundLight = Color(0xFFF9F9FF)
        override val onBackgroundLight = Color(0xFF191C20)
        override val surfaceLight = Color(0xFFF9F9FF)
        override val onSurfaceLight = Color(0xFF191C20)
        override val surfaceVariantLight = Color(0xFFE0E2EC)
        override val onSurfaceVariantLight = Color(0xFF44474E)
        override val outlineLight = Color(0xFF74777F)
        override val outlineVariantLight = Color(0xFFC4C6D0)
        override val scrimLight = Color(0xFF000000)
        override val inverseSurfaceLight = Color(0xFF2E3036)
        override val inverseOnSurfaceLight = Color(0xFFF0F0F7)
        override val inversePrimaryLight = Color(0xFFAAC7FF)
        override val surfaceDimLight = Color(0xFFD9D9E0)
        override val surfaceBrightLight = Color(0xFFF9F9FF)
        override val surfaceContainerLowestLight = Color(0xFFFFFFFF)
        override val surfaceContainerLowLight = Color(0xFFF3F3FA)
        override val surfaceContainerLight = Color(0xFFEDEDF4)
        override val surfaceContainerHighLight = Color(0xFFE7E8EE)
        override val surfaceContainerHighestLight = Color(0xFFE2E2E9)

        override val primaryDark = Color(0xFFAAC7FF)
        override val onPrimaryDark = Color(0xFF0A305F)
        override val primaryContainerDark = Color(0xFF284777)
        override val onPrimaryContainerDark = Color(0xFFD6E3FF)
        override val secondaryDark = Color(0xFFBEC6DC)
        override val onSecondaryDark = Color(0xFF283141)
        override val secondaryContainerDark = Color(0xFF3E4759)
        override val onSecondaryContainerDark = Color(0xFFDAE2F9)
        override val tertiaryDark = Color(0xFFDDBCE0)
        override val onTertiaryDark = Color(0xFF3F2844)
        override val tertiaryContainerDark = Color(0xFF573E5C)
        override val onTertiaryContainerDark = Color(0xFFFAD8FD)
        override val errorDark = Color(0xFFFFB4AB)
        override val onErrorDark = Color(0xFF690005)
        override val errorContainerDark = Color(0xFF93000A)
        override val onErrorContainerDark = Color(0xFFFFDAD6)
        override val backgroundDark = Color(0xFF111318)
        override val onBackgroundDark = Color(0xFFE2E2E9)
        override val surfaceDark = Color(0xFF111318)
        override val onSurfaceDark = Color(0xFFE2E2E9)
        override val surfaceVariantDark = Color(0xFF44474E)
        override val onSurfaceVariantDark = Color(0xFFC4C6D0)
        override val outlineDark = Color(0xFF8E9099)
        override val outlineVariantDark = Color(0xFF44474E)
        override val scrimDark = Color(0xFF000000)
        override val inverseSurfaceDark = Color(0xFFE2E2E9)
        override val inverseOnSurfaceDark = Color(0xFF2E3036)
        override val inversePrimaryDark = Color(0xFF415F91)
        override val surfaceDimDark = Color(0xFF111318)
        override val surfaceBrightDark = Color(0xFF37393E)
        override val surfaceContainerLowestDark = Color(0xFF0C0E13)
        override val surfaceContainerLowDark = Color(0xFF191C20)
        override val surfaceContainerDark = Color(0xFF1D2024)
        override val surfaceContainerHighDark = Color(0xFF282A2F)
        override val surfaceContainerHighestDark = Color(0xFF33353A)
    }
}
