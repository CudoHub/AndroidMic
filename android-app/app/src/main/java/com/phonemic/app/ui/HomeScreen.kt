package com.phonemic.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.unit.dp
import com.phonemic.app.service.SessionState
import com.phonemic.app.vm.DiscoveredPc
import com.phonemic.app.vm.MainViewModel

@Composable
fun HomeScreen(vm: MainViewModel) {
    val session by vm.session.collectAsState()
    val discovered by vm.discovered.collectAsState()
    val discovering by vm.discovering.collectAsState()
    val toast by vm.toast.collectAsState()
    val snackbar = remember { SnackbarHostState() }

    var host by rememberSaveable { mutableStateOf(vm.prefs.host) }
    var port by rememberSaveable { mutableStateOf(vm.prefs.port.toString()) }
    var token by rememberSaveable { mutableStateOf(vm.prefs.token) }

    LaunchedEffect(toast) {
        if (toast.isNotEmpty()) {
            snackbar.showSnackbar(toast)
            vm.onToastShown()
        }
    }

    val streaming = session.state == SessionState.STREAMING
    val busy = session.state in setOf(SessionState.CONNECTING, SessionState.AUTH, SessionState.WAITING_BT)

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        // --- Статус ---
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    StatusDot(session.state)
                    Spacer(Modifier.width(8.dp))
                    Text(statusText(session), style = MaterialTheme.typography.titleMedium)
                }
                if (session.error.isNotEmpty()) {
                    Text(session.error, color = MaterialTheme.colorScheme.error,
                        style = MaterialTheme.typography.bodySmall)
                }
                if (session.state != SessionState.IDLE) {
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        StatChip("RTT", "${session.rttMs} мс")
                        StatChip("Потери", "%.1f %%".format(session.lossPct))
                        if (session.batteryPct >= 0) StatChip("Батарея", "${session.batteryPct} %")
                        StatChip("Хост", session.host.ifEmpty { "—" })
                    }
                    LevelMeter(session.level)
                }
            }
        }

        // --- Живое управление ---
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                var volume by rememberSaveable { mutableStateOf(1.0f) }
                VolumeSlider(volume = volume, enabled = streaming, onChange = { volume = it; vm.setVolume(it) })
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    MuteButton(session.muted, enabled = session.state != SessionState.IDLE) { vm.toggleMute() }
                    PowerButton(streaming, enabled = !busy, onClick = { if (streaming || session.state == SessionState.ERROR) vm.stop() else vm.start() })
                }
            }
        }

        // --- Транспорт ---
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text("Транспорт", style = MaterialTheme.typography.titleSmall)
                val items = listOf(
                    "wifi" to "Wi-Fi", "usb" to "USB", "wfd" to "Wi-Fi Direct", "bt" to "Bluetooth"
                )
                SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
                    items.forEachIndexed { i, (id, label) ->
                        SegmentedButton(
                            selected = vm.prefs.transport == id,
                            onClick = { if (!busy && !streaming) vm.saveTransport(id) },
                            shape = SegmentedButtonDefaults.itemShape(i, items.size)
                        ) { Text(label, style = MaterialTheme.typography.labelSmall) }
                    }
                }
                if (vm.prefs.transport == "wfd") {
                    Text(
                        "Подключите телефон к группе ПК «DIRECT-…» и включите микрофон. Пароль группы — на странице «Транспорт» приложения ПК.",
                        style = MaterialTheme.typography.bodySmall
                    )
                }
                if (vm.prefs.transport == "bt") {
                    Text(
                        "Сопрягите телефон с ПК, включите микрофон и нажмите «Подключиться» в приложении ПК на странице Bluetooth.",
                        style = MaterialTheme.typography.bodySmall
                    )
                }
                if (vm.prefs.transport == "usb") {
                    Text(
                        "Включите USB-модем (tethering) либо USB-отладку + «Настроить adb reverse» в приложении ПК.",
                        style = MaterialTheme.typography.bodySmall
                    )
                }
            }
        }

        // --- Wi-Fi: discovery + ручной ввод ---
        if (vm.prefs.transport == "wifi" || vm.prefs.transport == "usb") {
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    Text("Подключение", style = MaterialTheme.typography.titleSmall)
                    OutlinedButton(onClick = { vm.discover() }, enabled = !discovering && !busy) {
                        Text(if (discovering) "Поиск…" else "Искать ПК в сети")
                    }
                    if (discovered.isNotEmpty()) {
                        Text("Найденные ПК", style = MaterialTheme.typography.labelLarge)
                        discovered.forEach { pc -> PcRow(pc) { vm.pickPc(it); host = it.address; port = it.port.toString() } }
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedTextField(
                            value = host, onValueChange = { host = it; vm.saveHost(it) },
                            label = { Text("Адрес ПК") }, modifier = Modifier.weight(1f),
                            singleLine = true, enabled = !busy
                        )
                        OutlinedTextField(
                            value = port, onValueChange = { port = it; it.toIntOrNull()?.let(vm::savePort) },
                            label = { Text("Порт") }, modifier = Modifier.width(110.dp),
                            singleLine = true,
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                            enabled = !busy
                        )
                    }
                    OutlinedTextField(
                        value = token, onValueChange = { token = it; vm.saveToken(it) },
                        label = { Text("Токен доступа (из приложения ПК)") },
                        modifier = Modifier.fillMaxWidth(), singleLine = true,
                        visualTransformation = PasswordVisualTransformation(),
                        enabled = !busy
                    )
                }
            }
        }
        if (vm.prefs.transport == "bt" || vm.prefs.transport == "wfd") {
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp)) {
                    OutlinedTextField(
                        value = token, onValueChange = { token = it; vm.saveToken(it) },
                        label = { Text("Токен доступа (из приложения ПК)") },
                        modifier = Modifier.fillMaxWidth(), singleLine = true,
                        visualTransformation = PasswordVisualTransformation(),
                        enabled = !busy
                    )
                }
            }
        }

        Spacer(Modifier.height(24.dp))
    }

    SnackbarHost(hostState = snackbar)

    // --- TOFU-диалог ---
    val pending = session.pendingPin
    if (pending.isNotEmpty()) {
        AlertDialog(
            onDismissRequest = {},
            title = { Text("Подтверждение ПК") },
            text = {
                Text("Отпечаток сертификата ПК:\n\n${pending.chunked(2).joinToString(" ")}\n\nСверьте его со страницей «Безопасность» приложения PhoneMic на компьютере.")
            },
            confirmButton = {
                Button(onClick = { vm.confirmPin(true) }) { Text("Совпадает") }
            },
            dismissButton = {
                OutlinedButton(onClick = { vm.confirmPin(false) }) { Text("Не совпадает") }
            }
        )
    }
}

@Composable
private fun PcRow(pc: DiscoveredPc, onClick: (DiscoveredPc) -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Row(
            Modifier.padding(12.dp).fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(Modifier.weight(1f)) {
                Text(pc.name, style = MaterialTheme.typography.bodyLarge)
                Text("${pc.address}:${pc.port}", style = MaterialTheme.typography.bodySmall)
            }
            Button(onClick = { onClick(pc) }) { Text("Выбрать") }
        }
    }
}

private fun statusText(s: com.phonemic.app.service.SessionUiState): String = when (s.state) {
    SessionState.IDLE -> "Остановлено"
    SessionState.CONNECTING -> "Подключение…"
    SessionState.AUTH -> "Авторизация…"
    SessionState.WAITING_BT -> "Ожидание ПК (Bluetooth)…"
    SessionState.STREAMING -> "В эфире"
    SessionState.ERROR -> "Ошибка"
}
