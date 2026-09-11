package com.phonemic.app.service

import android.app.Notification
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.BatteryManager
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import com.phonemic.app.App
import com.phonemic.app.MainActivity
import com.phonemic.app.R
import com.phonemic.app.audio.CaptureEngine
import com.phonemic.app.audio.OpusEncoderWrapper
import com.phonemic.app.core.AppLog
import com.phonemic.app.core.Constants
import com.phonemic.app.core.Prefs
import com.phonemic.app.crypto.Crypto
import com.phonemic.app.net.BluetoothServer
import com.phonemic.app.net.ControlBtChannel
import com.phonemic.app.net.ControlChannel
import com.phonemic.app.net.MediaRfcommChannel
import com.phonemic.app.net.MediaStreamPacker
import com.phonemic.app.net.MediaTcpChannel
import com.phonemic.app.net.MediaUdpChannel
import com.phonemic.app.net.ProtocolException
import com.phonemic.app.net.PinnedTrustManager
import com.phonemic.app.transport.BluetoothTransport
import com.phonemic.app.transport.ConnectionInfo
import com.phonemic.app.transport.Transport
import com.phonemic.app.transport.TransportException
import com.phonemic.app.transport.UsbTransport
import com.phonemic.app.transport.WifiDirectTransport
import com.phonemic.app.transport.WifiTransport
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.IOException

// ---------------- Состояние сессии для UI ----------------

enum class SessionState { IDLE, CONNECTING, AUTH, WAITING_BT, STREAMING, ERROR }

data class SessionUiState(
    val state: SessionState = SessionState.IDLE,
    val transportId: String = "",
    val host: String = "",
    val rttMs: Int = 0,
    val lossPct: Float = 0f,
    val batteryPct: Int = -1,
    val level: Float = 0f,
    val muted: Boolean = false,
    val error: String = "",
    /** Отпечаток сертификата ПК, требующий подтверждения (TOFU). */
    val pendingPin: String = ""
)

/**
 * Синглтон-сессия: жизненный цикл стриминга не зависит от Activity.
 * StreamingService держит Foreground Service и уведомление, вся логика — здесь.
 */
object SessionController {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private var sessionJob: Job? = null
    private var engine: CaptureEngine? = null
    private var wakeLock: PowerManager.WakeLock? = null

    private val _state = kotlinx.coroutines.flow.MutableStateFlow(SessionUiState())
    val state: kotlinx.coroutines.flow.StateFlow<SessionUiState> = _state

    // подтверждение TOFU
    private var pinConfirmed: Boolean? = null

    fun start(context: Context) {
        if (sessionJob?.isActive == true) return
        StreamingService.startForegroundService(context)
        val prefs = Prefs.get(context)
        sessionJob = scope.launch {
            var attempt = 0
            while (isActive) {
                try {
                    update { it.copy(state = SessionState.CONNECTING, error = "") }
                    runSession(context.applicationContext, prefs)
                    break // runSession вернулся после штатного stop
                } catch (e: ProtocolException) {
                    if (e.code == "busy") {
                        fail(context, "ПК занят другой сессией")
                        break
                    }
                    if (e.code == "bad_token") {
                        fail(context, "Неверный токен")
                        break
                    }
                    retryOrBreak(context, prefs, ++attempt, "сервер: ${e.code}")
                } catch (e: TransportException) {
                    retryOrBreak(context, prefs, ++attempt, e.message ?: "транспорт")
                } catch (e: Exception) {
                    retryOrBreak(context, prefs, ++attempt, e.message ?: e.javaClass.simpleName)
                }
            }
        }
    }

    fun stop(context: Context) {
        AppLog.i("Session", "stop by user")
        sessionJob?.cancel()
        sessionJob = null
        engine?.stop(); engine = null
        releaseWakeLock()
        update { it.copy(state = SessionState.IDLE, level = 0f, host = "") }
        StreamingService.stopService(context)
    }

    fun setVolume(context: Context, value: Float) {
        val v = value.coerceIn(0f, 1.5f)
        volumeCommandSink?.invoke(v)
    }

    fun toggleMute(context: Context): Boolean {
        val now = !(engine?.muted ?: _state.value.muted)
        engine?.setMuted(now)
        packerMutedSink?.invoke(now)
        muteCommandSink?.invoke(now)
        update { it.copy(muted = now) }
        return now
    }

    fun confirmPin(context: Context, accepted: Boolean) {
        pinConfirmed = accepted
        if (accepted) {
            Prefs.get(context).certPin = pendingPinValue
        }
        pendingPinValue = ""
    }

    // каналы команд, устанавливаемые активной сессией
    private var volumeCommandSink: ((Float) -> Unit)? = null
    private var muteCommandSink: ((Boolean) -> Unit)? = null
    private var packerMutedSink: ((Boolean) -> Unit)? = null
    private var pendingPinValue: String = ""

    private fun update(f: (SessionUiState) -> SessionUiState) {
        _state.value = f(_state.value)
    }

    private fun fail(context: Context, msg: String) {
        AppLog.e("Session", "fatal: $msg")
        update { it.copy(state = SessionState.ERROR, error = msg) }
        StreamingService.stopService(context)
    }

    private suspend fun retryOrBreak(context: Context, prefs: Prefs, attempt: Int, why: String) {
        AppLog.w("Session", "attempt $attempt failed: $why")
        if (!prefs.autoRestart) {
            fail(context, why)
            return
        }
        update { it.copy(state = SessionState.CONNECTING, error = "$why — переподключение…") }
        delay(minOf(1000L * attempt, Constants.RECONNECT_MAX_MS))
    }

    private suspend fun runSession(context: Context, prefs: Prefs) {
        val tokenRaw = try {
            Crypto.b64uDecode(prefs.token)
        } catch (e: Exception) {
            throw TransportException("Токен повреждён")
        }
        if (tokenRaw.size != 32) throw TransportException("Токен должен быть 43 символа base64url")

        // ---------------- Bluetooth: отдельный путь ----------------
        if (prefs.transport == "bt") {
            runBtSession(context, prefs, tokenRaw)
            return
        }

        // ---------------- IP-транспорты ----------------
        val transport: Transport = when (prefs.transport) {
            "usb" -> UsbTransport()
            "wfd" -> WifiDirectTransport(prefs.wfdPassphrase)
            else -> WifiTransport(prefs.host)
        }
        update { it.copy(transportId = transport.id) }
        val conn: ConnectionInfo = transport.connect(context, prefs.port)
        AppLog.i("Session", "transport ${transport.id} → ${conn.host}:${conn.port}")
        update { it.copy(host = conn.host) }

        // TLS control
        val ctl = ControlChannel(conn.host, conn.port, tokenRaw, prefs.certPin.ifEmpty { null })
        try {
            ctl.connect()
        } catch (e: Exception) {
            ctl.close()
            val msg = e.message ?: ""
            if (msg.startsWith("PIN_MISMATCH")) {
                throw TransportException("Отпечаток сертификата ПК изменился! Сбросьте пин в настройках, если это действительно ваш ПК.")
            }
            throw TransportException("Подключение не удалось: ${e.javaClass.simpleName}")
        }

        try {
            update { it.copy(state = SessionState.AUTH) }
            val ok = ctl.handshake(BuildConfig.VERSION_NAME, Build.MODEL, Build.VERSION.RELEASE,
                listOf("wifi", "usb", "wfd", "bt"))

            // TOFU-подтверждение отпечатка
            val serverFp = ok.optString("cert_fp")
            val presented = ctl.presentedFingerprint
            if (prefs.certPin.isEmpty()) {
                pinConfirmed = null
                pendingPinValue = serverFp.ifEmpty { presented ?: "" }
                update { it.copy(state = SessionState.AUTH, pendingPin = pendingPinValue) }
                while (pinConfirmed == null && kotlinx.coroutines.currentCoroutineContext().isActive) delay(100)
                if (pinConfirmed != true) {
                    ctl.sendBye("pin_rejected")
                    update { it.copy(state = SessionState.IDLE, pendingPin = "") }
                    return
                }
                update { it.copy(pendingPin = "") }
            } else if (presented != null && serverFp.isNotEmpty() &&
                !presented.equals(serverFp, ignoreCase = true)) {
                throw TransportException("Сервер изменил сертификат во время сессии")
            }

            // медиа-ключи
            val mediaSalt = Crypto.b64uDecode(ok.getString("media_salt"))
            val keys = Crypto.MediaKeys.derive(tokenRaw, mediaSalt)
            val mediaMode = ok.optString("media_mode", "udp")
            val mediaPort = ok.optInt("media_port", Constants.MEDIA_PORT_DEFAULT)

            // медиа-канал
            val media: MediaUdpChannel? = if (mediaMode == "udp") {
                MediaUdpChannel(java.net.InetAddress.getByName(conn.host), mediaPort)
            } else null
            val mediaTcp: MediaTcpChannel? = if (mediaMode == "tcp") {
                val tm = PinnedTrustManager(prefs.certPin.ifEmpty { null })
                MediaTcpChannel(conn.host, mediaPort, tm)
            } else null

            // поток
            val useOpus = prefs.codec == "opus"
            val frameMs = if (useOpus) prefs.frameMs else Constants.FRAME_MS_PCM
            ctl.sendStartStream(
                codec = if (useOpus) "opus" else "pcm",
                frameMs = frameMs,
                bitrateKbps = prefs.bitrateKbps,
                mode = "voice",
                aec = prefs.aec, agc = prefs.agc, ns = prefs.ns,
                gain = 1.0f
            )
            val started = ctl.receive()
            if (started.optString("t") != "stream_started") {
                throw ProtocolException(started.optString("code", "invalid_state"))
            }

            startCaptureLoop(context, prefs, ctl, keys, media, mediaTcp, useOpus, conn.host)
        } finally {
            ctl.close()
        }
    }

    // ---------------- Bluetooth-сессия ----------------

    private suspend fun runBtSession(context: Context, prefs: Prefs, tokenRaw: ByteArray) {
        val adapter = BluetoothTransport.getAdapter(context)
            ?: throw TransportException("Bluetooth недоступен")
        if (!adapter.isEnabled) throw TransportException("Bluetooth выключен")
        update { it.copy(state = SessionState.WAITING_BT, transportId = "bt", host = "Bluetooth") }

        var ctlSocket: android.bluetooth.BluetoothSocket? = null
        var mediaSocket: android.bluetooth.BluetoothSocket? = null
        val server = BluetoothServer()
        val accepted = kotlinx.coroutines.CompletableDeferred<Boolean>()
        server.start(adapter, object : BluetoothServer.Listener {
            override fun onCtlConnected(ctl: android.bluetooth.BluetoothSocket) { ctlSocket = ctl }
            override fun onMediaConnected(media: android.bluetooth.BluetoothSocket) {
                mediaSocket = media
                accepted.complete(true)
            }
            override fun onError(msg: String) {
                if (!accepted.isCompleted) accepted.complete(false)
                update { it.copy(error = "Bluetooth: $msg") }
            }
        })
        try {
            val okMedia = accepted.await()
            val ctlS = ctlSocket ?: throw TransportException("ПК не подключился (control)")
            if (!okMedia) throw TransportException("ПК не открыл медиа-канал")

            val ctl = ControlBtChannel(ctlS.inputStream, ctlS.outputStream)
            update { it.copy(state = SessionState.AUTH) }
            ctl.send(JSONObject()
                .put("t", "hello").put("proto", Constants.PROTO_VERSION)
                .put("app", "PhoneMic-Android").put("app_ver", BuildConfig.VERSION_NAME)
                .put("device", Build.MODEL).put("android", Build.VERSION.RELEASE)
                .put("transports", org.json.JSONArray(listOf("bt")))
                .put("caps", JSONObject().put("opus", true).put("pcm", true)
                    .put("aec", prefs.aec).put("agc", prefs.agc).put("ns", prefs.ns).put("bt", true)))
            val challenge = ctl.receive()
            if (challenge.optString("t") != "challenge") throw ProtocolException("expected challenge")
            ctl.send(JSONObject().put("t", "auth")
                .put("mac", Crypto.b64uEncode(Crypto.authMac(tokenRaw, Crypto.b64uDecode(challenge.getString("nonce"))))))
            val ok = ctl.receive()
            if (ok.optString("t") != "ok") throw ProtocolException(ok.optString("code", "internal"))

            // переключаемся на шифрованный control
            ctl.enableEncryption(tokenRaw, Crypto.b64uDecode(ok.getString("media_salt")))
            val keys = Crypto.MediaKeys.derive(tokenRaw, Crypto.b64uDecode(ok.getString("media_salt")))

            val media = MediaRfcommChannel(mediaSocket!!.outputStream, mediaSocket!!.inputStream)

            val useOpus = prefs.codec == "opus" // по BT только Opus
            ctl.send(JSONObject()
                .put("t", "start_stream")
                .put("codec", "opus").put("frame_ms", 20)
                .put("bitrate_kbps", minOf(prefs.bitrateKbps, 64))
                .put("mode", "voice")
                .put("aec", prefs.aec).put("agc", prefs.agc).put("ns", prefs.ns)
                .put("gain", 1.0))
            val started = ctl.receive()
            if (started.optString("t") != "stream_started") throw ProtocolException("invalid_state")

            startBtCaptureLoop(context, prefs, ctl, media, keys)
        } finally {
            server.stop()
            try { ctlSocket?.close() } catch (_: Exception) {}
            try { mediaSocket?.close() } catch (_: Exception) {}
        }
    }

    // ---------------- Цикл захвата ----------------

    private suspend fun startCaptureLoop(
        context: Context,
        prefs: Prefs,
        ctl: ControlChannel,
        keys: Crypto.MediaKeys,
        udp: MediaUdpChannel?,
        tcp: MediaTcpChannel?,
        useOpus: Boolean,
        host: String
    ) {
        acquireWakeLock(context)
        val cap = CaptureEngine(prefs.aec, prefs.agc, prefs.ns)
        engine = cap
        val encoder = if (useOpus) try {
            OpusEncoderWrapper(prefs.bitrateKbps)
        } catch (e: Exception) {
            AppLog.w("Session", "opus encoder failed → PCM")
            null
        } else null

        val packer = MediaStreamPacker(keys, if (encoder != null) Constants.PTYPE_OPUS else Constants.PTYPE_PCM)
        var pktTotal = 0L

        volumeCommandSink = { v -> try { ctl.sendCommand("set_volume", v) } catch (_: Exception) {} }
        muteCommandSink = { m -> try { ctl.sendCommand("set_mute", m) } catch (_: Exception) {} }
        packerMutedSink = { m -> packer.muted = m }

        update { it.copy(state = SessionState.STREAMING, muted = cap.muted, error = "") }

        // reader control-сообщений (pong/stats/event)
        val readerJob = scope.launch {
            while (isActive) {
                try {
                    val msg = ctl.receive()
                    when (msg.optString("t")) {
                        "pong" -> {
                            val tsC = msg.optLong("ts_client", 0)
                            if (tsC > 0) update { it.copy(rttMs = (System.currentTimeMillis() - tsC).toInt().coerceAtLeast(0)) }
                        }
                        "event" -> AppLog.i("Ctl", "event: ${msg.optString("code")} ${msg.optString("msg")}")
                        "stats" -> update {
                            it.copy(lossPct = calcLoss(msg.optInt("pkt_total"), msg.optInt("pkt_lost")))
                        }
                    }
                } catch (e: Exception) {
                    if (sessionJob?.isActive == true) AppLog.w("Ctl", "reader: ${e.message}")
                    break
                }
            }
        }

        // ping
        val pingJob = scope.launch {
            var id = 1L
            while (isActive) {
                delay(Constants.PING_INTERVAL_MS)
                try { ctl.sendPing(id++, System.currentTimeMillis()) } catch (_: Exception) { break }
            }
        }

        // stats от телефона
        val statsJob = scope.launch {
            while (isActive) {
                delay(Constants.STATS_INTERVAL_MS)
                try {
                    val bm = context.getSystemService(Context.BATTERY_SERVICE) as BatteryManager
                    val bat = bm.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY)
                    update { it.copy(batteryPct = bat) }
                    ctl.send(JSONObject()
                        .put("t", "stats")
                        .put("battery_pct", bat)
                        .put("pkt_total", pktTotal)
                        .put("pkt_lost", 0)
                        .put("buffer_ms", 10))
                } catch (_: Exception) { break }
            }
        }

        val frameSamples = (Constants.SAMPLE_RATE * (if (encoder != null) prefs.frameMs else 10) / 1000).toLong()

        try {
            if (!cap.start()) throw TransportException("Микрофон недоступен (разрешение?)")
            while (kotlinx.coroutines.currentCoroutineContext().isActive) {
                val frame = cap.readFrame() ?: break
                if (encoder != null) {
                    val opusFrames = encoder.encode(frame)
                    for (f in opusFrames) {
                        val packet = packer.next(f, frameSamples)
                        if (udp != null) udp.send(packet) else tcp?.send(packet)
                        pktTotal++
                    }
                } else {
                    val packet = packer.next(frame, frameSamples)
                    if (udp != null) udp.send(packet) else tcp?.send(packet)
                    pktTotal++
                }
                val lvl = cap.rmsLevel
                if (kotlinx.coroutines.currentCoroutineContext().isActive) update { it.copy(level = lvl) }
            }
        } catch (e: IOException) {
            AppLog.w("Session", "media send failed: ${e.message}")
        } finally {
            readerJob.cancel(); pingJob.cancel(); statsJob.cancel()
            engine?.stop(); engine = null
            encoder?.stop()
            try { ctl.sendBye("app_stopped") } catch (_: Exception) {}
            udp?.close(); tcp?.close()
            volumeCommandSink = null; muteCommandSink = null; packerMutedSink = null
            releaseWakeLock()
        }
        // выход из цикла = разрыв; SessionController решает: переподключение или стоп
        throw TransportException("соединение прервано")
    }

    private suspend fun startBtCaptureLoop(
        context: Context,
        prefs: Prefs,
        ctl: ControlBtChannel,
        media: MediaRfcommChannel,
        keys: Crypto.MediaKeys
    ) {
        acquireWakeLock(context)
        val cap = CaptureEngine(prefs.aec, prefs.agc, prefs.ns)
        engine = cap
        val encoder = try { OpusEncoderWrapper(minOf(prefs.bitrateKbps, 64)) } catch (e: Exception) { null }
            ?: throw TransportException("Opus-кодировщик недоступен (для Bluetooth он обязателен)")

        val packer = MediaStreamPacker(keys, Constants.PTYPE_OPUS)
        volumeCommandSink = { v -> try { ctl.send(JSONObject().put("t", "cmd").put("cmd", "set_volume").put("value", v.toDouble())) } catch (_: Exception) {} }
        muteCommandSink = { m -> try { ctl.send(JSONObject().put("t", "cmd").put("cmd", "set_mute").put("value", m)) } catch (_: Exception) {} }
        packerMutedSink = { m -> packer.muted = m }
        update { it.copy(state = SessionState.STREAMING, muted = cap.muted) }

        val readerJob = scope.launch {
            while (isActive) {
                try {
                    val msg = ctl.receive()
                    if (msg.optString("t") == "pong") {
                        val tsC = msg.optLong("ts_client", 0)
                        if (tsC > 0) update { it.copy(rttMs = (System.currentTimeMillis() - tsC).toInt()) }
                    }
                } catch (e: Exception) { break }
            }
        }
        val pingJob = scope.launch {
            var id = 1L
            while (isActive) {
                delay(Constants.PING_INTERVAL_MS)
                try { ctl.send(JSONObject().put("t", "ping").put("id", id++).put("ts_client", System.currentTimeMillis())) } catch (_: Exception) { break }
            }
        }

        try {
            if (!cap.start()) throw TransportException("Микрофон недоступен (разрешение?)")
            val frameSamples = (Constants.SAMPLE_RATE * prefs.frameMs.coerceAtLeast(20) / 1000).toLong()
            while (kotlinx.coroutines.currentCoroutineContext().isActive) {
                val frame = cap.readFrame() ?: break
                for (f in encoder.encode(frame)) {
                    media.send(packer.next(f, frameSamples))
                }
                update { it.copy(level = cap.rmsLevel) }
            }
        } finally {
            readerJob.cancel(); pingJob.cancel()
            engine?.stop(); engine = null
            encoder.stop()
            volumeCommandSink = null; muteCommandSink = null; packerMutedSink = null
            releaseWakeLock()
        }
        throw TransportException("Bluetooth-соединение прервано")
    }

    private fun calcLoss(total: Int, lost: Int): Float =
        if (total <= 0) 0f else (lost * 100f / total).coerceIn(0f, 100f)

    private fun acquireWakeLock(context: Context) {
        if (wakeLock != null) return
        val pm = context.getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "phonemic:stream").apply {
            setReferenceCounted(false)
            acquire(4 * 60 * 60 * 1000L)
        }
    }

    private fun releaseWakeLock() {
        try { wakeLock?.release() } catch (_: Exception) {}
        wakeLock = null
    }
}

// ---------------- Foreground Service ----------------

class StreamingService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_MUTE -> {
                SessionController.toggleMute(this)
            }
            ACTION_STOP -> {
                SessionController.stop(this)
                return START_NOT_STICKY
            }
            else -> {
                startAsForeground()
                // следим за состоянием, чтобы обновлять уведомление
                scope.launch {
                    SessionController.state.collect { st ->
                        if (st.state == SessionState.IDLE || st.state == SessionState.ERROR) {
                            stopForeground(STOP_FOREGROUND_REMOVE)
                            stopSelf()
                        } else {
                            notify(st)
                        }
                    }
                }
            }
        }
        return START_STICKY
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    private fun startAsForeground() {
        val n = buildNotification(SessionController.state.value)
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE)
        } else {
            startForeground(NOTIF_ID, n)
        }
    }

    private fun notify(st: SessionUiState) {
        val nm = getSystemService(android.app.NotificationManager::class.java)
        nm?.notify(NOTIF_ID, buildNotification(st))
    }

    private fun buildNotification(st: SessionUiState): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        val muteLabel = if (st.muted) getString(R.string.notif_action_unmute) else getString(R.string.notif_action_mute)
        val mutePi = PendingIntent.getService(
            this, 1, Intent(this, StreamingService::class.java).setAction(ACTION_MUTE),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        val stopPi = PendingIntent.getService(
            this, 2, Intent(this, StreamingService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        val text = when (st.state) {
            SessionState.STREAMING -> getString(R.string.notif_connected, st.host.ifEmpty { "ПК" })
            SessionState.WAITING_BT -> getString(R.string.notif_waiting)
            else -> st.error.ifEmpty { getString(R.string.notif_waiting) }
        }
        return android.app.Notification.Builder(this, App.CHANNEL_STREAM)
            .setSmallIcon(R.drawable.ic_phonemic)
            .setContentTitle(getString(R.string.notif_title))
            .setContentText(text)
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .addAction(android.R.drawable.ic_btn_speak_now, muteLabel, mutePi)
            .addAction(android.R.drawable.ic_media_pause, getString(R.string.notif_action_stop), stopPi)
            .build()
    }

    override fun onDestroy() {
        scope.cancel()
        super.onDestroy()
    }

    companion object {
        private const val NOTIF_ID = 1
        private const val ACTION_MUTE = "com.phonemic.app.MUTE"
        private const val ACTION_STOP = "com.phonemic.app.STOP"

        fun startForegroundService(context: Context) {
            val i = Intent(context, StreamingService::class.java)
            if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(i) else context.startService(i)
        }

        fun stopService(context: Context) {
            context.stopService(Intent(context, StreamingService::class.java))
        }
    }
}
