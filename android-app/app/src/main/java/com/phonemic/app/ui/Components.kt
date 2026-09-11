package com.phonemic.app.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.MicOff
import androidx.compose.material.icons.filled.PowerSettingsNew
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.unit.dp
import com.phonemic.app.service.SessionState

/** История уровня (столбики) — Canvas. */
@Composable
fun LevelMeter(level: Float, modifier: Modifier = Modifier) {
    val history = remember { ArrayDeque<Float>() }
    LaunchedEffect(level) {
        if (history.size > 60) history.removeFirst()
        history.addLast(level)
    }
    val primary = MaterialTheme.colorScheme.primary
    Canvas(modifier = modifier.height(48.dp).fillMaxWidth()) {
        val n = 60
        val w = size.width / n
        history.forEachIndexed { i, v ->
            val h = (v.coerceIn(0f, 1f)) * size.height * 0.95f
            drawLine(
                color = primary,
                start = Offset(i * w + w / 2, size.height),
                end = Offset(i * w + w / 2, size.height - h),
                strokeWidth = w * 0.7f,
                cap = StrokeCap.Round
            )
        }
    }
}

@Composable
fun StatusDot(state: SessionState) {
    val color = when (state) {
        SessionState.STREAMING -> Color(0xFF4CAF50)
        SessionState.CONNECTING, SessionState.AUTH, SessionState.WAITING_BT -> Color(0xFFFFC107)
        SessionState.ERROR -> Color(0xFFF44336)
        SessionState.IDLE -> Color(0xFF9E9E9E)
    }
    Box(modifier = Modifier.size(12.dp).background(color, CircleShape))
}

@Composable
fun StatChip(label: String, value: String) {
    AssistChip(onClick = {}, enabled = false, label = { Text("$label $value") })
}

@Composable
fun VolumeSlider(volume: Float, enabled: Boolean, onChange: (Float) -> Unit) {
    Column {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text("Громкость", style = MaterialTheme.typography.labelLarge)
            Text("${(volume * 100).toInt()} %", style = MaterialTheme.typography.labelLarge)
        }
        Slider(
            value = volume.coerceIn(0f, 1.5f),
            onValueChange = onChange,
            valueRange = 0f..1.5f,
            enabled = enabled
        )
    }
}

@Composable
fun PowerButton(streaming: Boolean, enabled: Boolean, onClick: () -> Unit) {
    Button(
        onClick = onClick,
        enabled = enabled,
        colors = if (streaming) ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error)
                 else ButtonDefaults.buttonColors()
    ) {
        Icon(if (streaming) Icons.Filled.PowerSettingsNew else Icons.Filled.Mic, contentDescription = null)
        Text(if (streaming) "  Выключить" else "  Включить микрофон")
    }
}

@Composable
fun MuteButton(muted: Boolean, enabled: Boolean, onClick: () -> Unit) {
    Button(onClick = onClick, enabled = enabled) {
        Icon(if (muted) Icons.Filled.MicOff else Icons.Filled.Mic, contentDescription = null)
        Text(if (muted) "  Включить звук" else "  Мьют")
    }
}
