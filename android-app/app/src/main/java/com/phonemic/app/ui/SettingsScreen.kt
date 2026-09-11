package com.phonemic.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.phonemic.app.vm.MainViewModel

@Composable
fun SettingsScreen(vm: MainViewModel) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Text("Настройки", style = MaterialTheme.typography.headlineSmall)

        // --- Аудио ---
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Аудио", style = MaterialTheme.typography.titleSmall)

                Text("Кодек", style = MaterialTheme.typography.labelLarge)
                SingleChoice(vm.prefs.codec == "opus", "Opus (рекомендуется)") { vm.saveCodec("opus") }
                SingleChoice(vm.prefs.codec == "pcm", "PCM 16-bit (lossless, только быстрые сети)") { vm.saveCodec("pcm") }

                HorizontalDivider()
                Text("Кадр Opus", style = MaterialTheme.typography.labelLarge)
                SingleChoice(vm.prefs.frameMs == 20, "20 мс (надёжно)") { vm.saveFrameMs(20) }
                SingleChoice(vm.prefs.frameMs == 10, "10 мс (меньше задержка, не на всех устройствах)") { vm.saveFrameMs(10) }

                HorizontalDivider()
                Text("Битрейт Opus", style = MaterialTheme.typography.labelLarge)
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    listOf(24, 32, 48, 64, 96).forEach { b ->
                        FilterChipSmall(
                            selected = vm.prefs.bitrateKbps == b,
                            label = "$b kbps"
                        ) { vm.saveBitrate(b) }
                    }
                }

                HorizontalDivider()
                SwitchRow("Эхоподавление (AEC)", vm.prefs.aec) { vm.saveAec(it) }
                SwitchRow("Автоусиление (AGC)", vm.prefs.agc) { vm.saveAgc(it) }
                SwitchRow("Шумоподавление (NS)", vm.prefs.ns) { vm.saveNs(it) }
            }
        }

        // --- Сеть ---
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Сеть", style = MaterialTheme.typography.titleSmall)
                Text("Медиа-канал", style = MaterialTheme.typography.labelLarge)
                SingleChoice(vm.prefs.mediaMode == "auto", "Авто (решает сервер)") { vm.saveMediaMode("auto") }
                SingleChoice(vm.prefs.mediaMode == "udp", "UDP (низкая задержка)") { vm.saveMediaMode("udp") }
                SingleChoice(vm.prefs.mediaMode == "tcp", "TCP/TLS (надёжно)") { vm.saveMediaMode("tcp") }
                Text(
                    "Внимание: финальный режим согласует сервер. Для adb reverse будет TCP.",
                    style = MaterialTheme.typography.bodySmall
                )
            }
        }

        // --- Поведение ---
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Поведение", style = MaterialTheme.typography.titleSmall)
                SwitchRow("Держать экран включённым", vm.prefs.keepScreenOn) { vm.saveKeepScreen(it) }
                SwitchRow("Автоперезапуск при разрыве", vm.prefs.autoRestart) { vm.saveAutoRestart(it) }
            }
        }

        // --- Безопасность ---
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Безопасность", style = MaterialTheme.typography.titleSmall)
                OutlinedButton(onClick = { vm.resetPin() }) {
                    Text("Сбросить отпечаток сертификата ПК")
                }
                Text(
                    "Сброс требуется только если вы сами переустановили сервер PhoneMic на ПК.",
                    style = MaterialTheme.typography.bodySmall
                )
            }
        }

        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp)) {
                Text("О приложении", style = MaterialTheme.typography.titleSmall)
                Text("PhoneMic 1.0.0 — телефон как микрофон для Windows. Протокол v1: TLS + токен + AES-256-GCM.", style = MaterialTheme.typography.bodySmall)
            }
        }
    }
}

@Composable
private fun SingleChoice(selected: Boolean, label: String, onClick: () -> Unit) {
    FilterChipSmall(selected, label, onClick)
}

@Composable
private fun FilterChipSmall(selected: Boolean, label: String, onClick: () -> Unit) {
    androidx.compose.material3.FilterChip(
        selected = selected,
        onClick = onClick,
        label = { Text(label, style = MaterialTheme.typography.labelSmall) }
    )
}

@Composable
private fun SwitchRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(
        Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(label, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
        Switch(checked = checked, onCheckedChange = onChange)
    }
}
