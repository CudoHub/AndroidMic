package com.phonemic.app.vm

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import com.phonemic.app.core.Prefs
import com.phonemic.app.net.Discovery
import com.phonemic.app.service.SessionController
import com.phonemic.app.service.SessionState
import com.phonemic.app.service.SessionUiState
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import androidx.lifecycle.viewModelScope

data class DiscoveredPc(val address: String, val name: String, val port: Int, val certFp: String)

class MainViewModel(app: Application) : AndroidViewModel(app) {

    val prefs = Prefs.get(app)
    val session: StateFlow<SessionUiState> = SessionController.state

    val sessionState: SessionState get() = session.value.state
    val isRunning: Boolean get() = session.value.state != SessionState.IDLE &&
        session.value.state != SessionState.ERROR

    private val _discovered = MutableStateFlow<List<DiscoveredPc>>(emptyList())
    val discovered: StateFlow<List<DiscoveredPc>> = _discovered

    private val _discovering = MutableStateFlow(false)
    val discovering: StateFlow<Boolean> = _discovering

    private val _toast = MutableStateFlow<String>("")
    val toast: StateFlow<String> = _toast

    fun onToastShown() { _toast.value = "" }

    fun start() {
        if (prefs.transport == "wifi" && prefs.host.isBlank()) {
            _toast.value = getApplication<Application>().getString(com.phonemic.app.R.string.err_no_host)
            return
        }
        if (prefs.token.isBlank()) {
            _toast.value = getApplication<Application>().getString(com.phonemic.app.R.string.err_no_token)
            return
        }
        SessionController.start(getApplication())
    }

    fun stop() = SessionController.stop(getApplication())

    fun toggleMute(): Boolean = SessionController.toggleMute(getApplication())

    fun setVolume(v: Float) = SessionController.setVolume(getApplication(), v)

    fun confirmPin(accepted: Boolean) {
        SessionController.confirmPin(getApplication(), accepted)
        if (!accepted) stop()
    }

    fun discover() {
        if (_discovering.value) return
        _discovering.value = true
        viewModelScope.launch {
            try {
                val found = Discovery.discover()
                _discovered.value = found.map { DiscoveredPc(it.address, it.name, it.port, it.certFp) }
            } catch (e: Exception) {
                _toast.value = "Discovery: ${e.message}"
            } finally {
                _discovering.value = false
            }
        }
    }

    fun pickPc(pc: DiscoveredPc) {
        prefs.host = pc.address
        prefs.port = pc.port
        // отпечаток предзаполняется только для сверки пользователем; пин сохранится после TOFU
    }

    fun saveTransport(id: String) { prefs.transport = id }
    fun saveHost(h: String) { prefs.host = h.trim() }
    fun savePort(p: Int) { if (p in 1..65535) prefs.port = p }
    fun saveToken(t: String) { prefs.token = t.trim() }
    fun saveCodec(c: String) { prefs.codec = c }
    fun saveFrameMs(f: Int) { prefs.frameMs = f }
    fun saveBitrate(b: Int) { prefs.bitrateKbps = b }
    fun saveAec(v: Boolean) { prefs.aec = v }
    fun saveAgc(v: Boolean) { prefs.agc = v }
    fun saveNs(v: Boolean) { prefs.ns = v }
    fun saveMediaMode(m: String) { prefs.mediaMode = m }
    fun saveKeepScreen(v: Boolean) { prefs.keepScreenOn = v }
    fun saveAutoRestart(v: Boolean) { prefs.autoRestart = v }
    fun resetPin() { prefs.resetPin() }
}
