package me.dabao1955.tamisu.ui.component

import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import me.dabao1955.tamisu.Natives

@Composable
fun TamisuIsValid(
    content: @Composable () -> Unit
) {
    // Cache the JNI roundtrip; manager status and tamisu version do not change
    // for the lifetime of this composition.
    val tamisuVersion = remember {
        if (Natives.isManager) Natives.version else null
    }
    if (tamisuVersion != null) {
        content()
    }
}