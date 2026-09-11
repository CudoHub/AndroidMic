package com.phonemic.app.transport

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.wifi.WpsInfo
import android.net.wifi.p2p.WifiP2pConfig
import android.net.wifi.p2p.WifiP2pDevice
import android.net.wifi.p2p.WifiP2pInfo
import android.net.wifi.p2p.WifiP2pManager
import com.phonemic.app.core.AppLog
import com.phonemic.app.core.Constants
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import java.net.InetSocketAddress
import java.net.Socket
import kotlin.coroutines.resume

/** Результат успешного транспорта: куда подключаться и подсказка по медиа. */
data class ConnectionInfo(
    val host: String,
    val port: Int,
    val mediaModeHint: String // "udp" | "tcp" | "" (решает сервер)
)

/** Ошибка транспорта с человекочитаемым сообщением. */
class TransportException(message: String) : Exception(message)

/** Транспорт: устанавливает L2/L3-связность и возвращает адрес control-сервера ПК. */
interface Transport {
    val id: String
    val displayName: String
    suspend fun connect(ctx: Context, port: Int): ConnectionInfo
}

/** Wi-Fi LAN: прямое подключение host:port. */
class WifiTransport(private val host: String) : Transport {
    override val id = "wifi"
    override val displayName = "Wi-Fi"

    override suspend fun connect(ctx: Context, port: Int): ConnectionInfo {
        if (host.isBlank()) throw TransportException("Укажите адрес ПК")
        val ok = withContext(Dispatchers.IO) {
            probe(host, port)
        }
        if (!ok) throw TransportException("ПК $host:$port недоступен")
        return ConnectionInfo(host, port, "")
    }

    companion object {
        /** Проверка достижимости control-порта. */
        fun probe(host: String, port: Int): Boolean = try {
            Socket().use { s ->
                s.tcpNoDelay = true
                s.connect(InetSocketAddress(host, port), 4000)
                true
            }
        } catch (e: Exception) {
            AppLog.w("Wifi", "probe $host:$port: ${e.message}")
            false
        }
    }
}

/**
 * USB: (a) USB-tethering — ПК = шлюз интерфейса usb*/rndis*; (b) adb reverse — 127.0.0.1.
 * Кандидаты пробуются по порядку.
 */
class UsbTransport : Transport {
    override val id = "usb"
    override val displayName = "USB"

    @SuppressLint("MissingPermission")
    override suspend fun connect(ctx: Context, port: Int): ConnectionInfo {
        val cm = ctx.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val candidates = ArrayList<String>()
        for (network in cm.allNetworks) {
            val lp: LinkProperties = cm.getLinkProperties(network) ?: continue
            val name = lp.interfaceName ?: continue
            if (name.startsWith("usb") || name.startsWith("rndis")) {
                // шлюз точка-точка ссылки — это ПК
                for (gl in lp.dnsServers) { /* dns не ПК, пропускаем */ }
                lp.gateway?.hostAddress?.let { candidates.add(it) }
                // фолбэк: адрес подсети .1
                for (la in lp.linkAddresses) {
                    val ip = la.address.hostAddress?.substringBefore('/') ?: continue
                    val base = ip.substringBeforeLast('.')
                    candidates.add("$base.1")
                    candidates.add("$base.2")
                }
            }
        }
        candidates.add("127.0.0.1") // adb reverse
        AppLog.i("USB", "candidates: $candidates")

        for (c in candidates.distinct()) {
            val mediaHint = if (c == "127.0.0.1") "tcp" else "udp"
            val ok = withContext(Dispatchers.IO) { WifiTransport.probe(c, port) }
            if (ok) {
                return ConnectionInfo(c, port, mediaHint)
            }
        }
        throw TransportException(
            "ПК по USB не найден. Включите USB-модем (тethering) в настройках телефона " +
            "или USB-отладку с разрешением «adb reverse» в приложении ПК."
        )
    }
}

/** Wi-Fi Direct: телефон подключается к группе, владельцем которой является ПК. */
class WifiDirectTransport(private val passphrase: String) : Transport {
    override val id = "wfd"
    override val displayName = "Wi-Fi Direct"

    @SuppressLint("MissingPermission")
    override suspend fun connect(ctx: Context, port: Int): ConnectionInfo = withContext(Dispatchers.Main) {
        val mgr = ctx.getSystemService(Context.WIFI_P2P_SERVICE) as? WifiP2pManager
            ?: throw TransportException("Wi-Fi Direct недоступен")
        val channel = mgr.initialize(ctx, ctx.mainLooper, null)

        // 1. discoverPeers
        val discovered = suspendCancellableCoroutine { cont ->
            mgr.discoverPeers(channel, object : WifiP2pManager.ActionListener {
                override fun onSuccess() {}
                override fun onFailure(reason: Int) {
                    if (cont.isActive) cont.resume(null)
                }
            })
            // peers приходят асинхронно; ждём через receiver ниже
            cont.invokeOnCancellation { }
        }
        // discoverPeers не возвращает список — слушаем peers дольше
        val peers: List<WifiP2pDevice> = awaitPeers(ctx, mgr, channel) ?: emptyList()
        AppLog.i("WFD", "peers: ${peers.map { it.deviceName }}")
        val target = peers.firstOrNull {
            it.deviceName.contains(Constants.NAME_PREFIX, ignoreCase = true) ||
            it.deviceName.contains("DIRECT", ignoreCase = true)
        } ?: throw TransportException("ПК (Wi-Fi Direct) не найден. Включите публикацию в приложении ПК.")

        // 2. connect (ПК — Group Owner, intent 0). Пароль WPA2 — через WifiP2pConfig.passphrase (API 33+);
        // на более старых Android система сама спросит пароль при подключении.
        val cfg = WifiP2pConfig().apply {
            deviceAddress = target.deviceAddress
            groupOwnerIntent = 0
            if (android.os.Build.VERSION.SDK_INT >= 33 && passphrase.isNotBlank()) {
                passphrase = this@WifiDirectTransport.passphrase
            } else {
                wps.setup = WpsInfo.PBC
            }
        }
        val connected = suspendCancellableCoroutine { cont ->
            mgr.connect(channel, cfg, object : WifiP2pManager.ActionListener {
                override fun onSuccess() {}
                override fun onFailure(reason: Int) {
                    if (cont.isActive) cont.resume(false)
                }
            })
        }
        if (connected == false) throw TransportException("Не удалось подключиться к группе Wi-Fi Direct")

        // 3. ждём GROUP_FORMED → groupOwnerAddress
        val info: WifiP2pInfo? = awaitConnectionInfo(ctx, mgr, channel)
            ?: throw TransportException("Группа Wi-Fi Direct не сформировалась (проверьте пароль)")
        val host = info.groupOwnerAddress?.hostAddress
            ?: throw TransportException("Не получен адрес ПК в группе")
        delay(1500) // дать DHCP завершиться
        val ok = withContext(Dispatchers.IO) { WifiTransport.probe(host, port) }
        if (!ok) throw TransportException("ПК $host:$port (Wi-Fi Direct) недоступен")
        ConnectionInfo(host, port, "udp")
    }

    @SuppressLint("MissingPermission")
    private suspend fun awaitPeers(
        ctx: Context, mgr: WifiP2pManager, channel: WifiP2pManager.Channel
    ): List<WifiP2pDevice>? = withTimeoutOrNull(15000) {
        suspendCancellableCoroutine { cont ->
            val receiver = object : android.content.BroadcastReceiver() {
                override fun onReceive(c: Context, intent: android.content.Intent) {
                    if (intent.action == WifiP2pManager.WIFI_P2P_PEERS_CHANGED_ACTION) {
                        @Suppress("DEPRECATION")
                        mgr.requestPeers(channel) { peers ->
                            val filtered = peers.deviceList.filter {
                                it.deviceName.contains(Constants.NAME_PREFIX, ignoreCase = true)
                            }
                            if (filtered.isNotEmpty() && cont.isActive) cont.resume(filtered)
                        }
                    }
                }
            }
            val filter = android.content.IntentFilter(WifiP2pManager.WIFI_P2P_PEERS_CHANGED_ACTION)
            ctx.registerReceiver(receiver, filter)
            cont.invokeOnCancellation { try { ctx.unregisterReceiver(receiver) } catch (_: Exception) {} }
        }
    }

    @SuppressLint("MissingPermission")
    private suspend fun awaitConnectionInfo(
        ctx: Context, mgr: WifiP2pManager, channel: WifiP2pManager.Channel
    ): WifiP2pInfo? = withTimeoutOrNull(30000) {
        suspendCancellableCoroutine { cont ->
            val receiver = object : android.content.BroadcastReceiver() {
                override fun onReceive(c: Context, intent: android.content.Intent) {
                    if (intent.action == WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION) {
                        mgr.requestConnectionInfo(channel) { info ->
                            if (info.groupFormed && cont.isActive) cont.resume(info)
                        }
                    }
                }
            }
            val filter = android.content.IntentFilter(WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION)
            ctx.registerReceiver(receiver, filter)
            cont.invokeOnCancellation { try { ctx.unregisterReceiver(receiver) } catch (_: Exception) {} }
        }
    }
}

/** Bluetooth: телефон — сервер; здесь только проверка адаптера/разрешений. */
class BluetoothTransport : Transport {
    override val id = "bt"
    override val displayName = "Bluetooth"

    override suspend fun connect(ctx: Context, port: Int): ConnectionInfo {
        val adapter = (ctx.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
            ?: throw TransportException("Bluetooth недоступен")
        if (!adapter.isEnabled) throw TransportException("Bluetooth выключен")
        // Для BT "connect" ничего не открывает: RFCOMM-серверы поднимет StreamingService.
        return ConnectionInfo("127.0.0.1", port, "bt")
    }

    companion object {
        fun getAdapter(ctx: Context): BluetoothAdapter? =
            (ctx.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter
    }
}
