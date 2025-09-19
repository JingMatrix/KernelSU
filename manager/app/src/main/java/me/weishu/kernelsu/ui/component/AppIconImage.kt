package me.weishu.kernelsu.ui.component

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.graphics.drawable.toBitmap
import com.kyant.capsule.ContinuousRoundedRectangle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import me.weishu.kernelsu.ui.viewmodel.SuperUserViewModel
import top.yukonga.miuix.kmp.theme.MiuixTheme.colorScheme

@Composable
fun AppIconImage(
    app: SuperUserViewModel.AppInfo,
    modifier: Modifier = Modifier
) {
    val context = LocalContext.current
    val pm = context.packageManager
    var icon by remember(app.packageName, app.user) { mutableStateOf<ImageBitmap?>(null) }

    LaunchedEffect(app.packageName, app.user) {
        withContext(Dispatchers.IO) {
            val rawIcon = app.packageInfo.applicationInfo?.loadIcon(pm)

            val finalIcon = rawIcon?.let {
                // This is the key change: get the system-badged icon
                pm.getUserBadgedIcon(it, app.user)
            }

            val bitmap = finalIcon?.toBitmap()?.asImageBitmap()
            icon = bitmap
        }
    }

    icon?.let { imageBitmap ->
        Image(
            bitmap = imageBitmap,
            contentDescription = app.label,
            modifier = modifier
        )
    } ?: Box(
        modifier = modifier
            .clip(ContinuousRoundedRectangle(12.dp))
            .background(colorScheme.secondaryContainer),
        contentAlignment = Alignment.Center
    ) {}
}
