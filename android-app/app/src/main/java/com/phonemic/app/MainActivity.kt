package com.phonemic.app

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.core.content.ContextCompat
import androidx.lifecycle.viewmodel.compose.viewModel
import com.phonemic.app.ui.HomeScreen
import com.phonemic.app.ui.PhoneMicTheme
import com.phonemic.app.ui.SettingsScreen
import com.phonemic.app.vm.MainViewModel

class MainActivity : ComponentActivity() {

    private val permLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { /* результат не важен: старт проверит RECORD_AUDIO */ }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        setContent {
            PhoneMicTheme {
                val vm: MainViewModel = viewModel()
                var tab by remember { mutableStateOf(0) }

                // держим экран включённым при стриминге
                val sessionState by vm.session.collectAsStateSafe()
                window.addFlags(
                    if (vm.prefs.keepScreenOn) WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
                    else 0
                )

                Scaffold(
                    bottomBar = {
                        NavigationBar {
                            NavigationBarItem(
                                selected = tab == 0, onClick = { tab = 0 },
                                icon = { Icon(androidx.compose.material.icons.Icons.Filled.Mic, null) },
                                label = { Text(getString(R.string.tab_home)) }
                            )
                            NavigationBarItem(
                                selected = tab == 1, onClick = { tab = 1 },
                                icon = { Icon(androidx.compose.material.icons.Icons.Filled.Settings, null) },
                                label = { Text(getString(R.string.tab_settings)) }
                            )
                        }
                    }
                ) { padding ->
                    androidx.compose.foundation.layout.Box(
                        modifier = androidx.compose.ui.Modifier
                            .padding(padding)
                    ) {
                        if (tab == 0) HomeScreen(vm) else SettingsScreen(vm)
                    }
                }
            }
        }

        requestNeededPermissions()
    }

    private fun requestNeededPermissions() {
        val wanted = mutableListOf(
            Manifest.permission.RECORD_AUDIO,
            Manifest.permission.ACCESS_FINE_LOCATION
        )
        if (Build.VERSION.SDK_INT >= 31) {
            wanted += listOf(
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.BLUETOOTH_SCAN
            )
        }
        if (Build.VERSION.SDK_INT >= 33) {
            wanted += listOf(
                Manifest.permission.POST_NOTIFICATIONS,
                Manifest.permission.NEARBY_WIFI_DEVICES
            )
        }
        val missing = wanted.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isNotEmpty()) permLauncher.launch(missing.toTypedArray())
    }
}

/** collectAsState, безопасный для использования до первой рекомпозиции. */
@androidx.compose.runtime.Composable
private fun <T> kotlinx.coroutines.flow.StateFlow<T>.collectAsStateSafe() =
    androidx.compose.runtime.collectAsState(initial = value)
