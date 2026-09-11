package com.phonemic.app.net

import com.phonemic.app.core.AppLog
import com.phonemic.app.core.Constants
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocket

/** Медиа-канал поверх UDP: send-only с телефона. */
class MediaUdpChannel(private val remoteAddr: InetAddress, private val remotePort: Int) : AutoCloseable {
    private val socket = DatagramSocket(null).apply {
        reuseAddress = true
        bind(InetSocketAddress(0))
    }

    @Throws(IOException::class)
    fun send(packet: ByteArray) {
        val dp = DatagramPacket(packet, packet.size, remoteAddr, remotePort)
        socket.send(dp)
    }

    override fun close() = socket.close()
}

/** Медиа-канал поверх TCP+TLS (используется для adb reverse и режима «надёжно»). */
class MediaTcpChannel(host: String, port: Int, trustManager: javax.net.ssl.TrustManager) : AutoCloseable {
    private val socket: SSLSocket
    private val output: OutputStream

    init {
        val raw = Socket()
        raw.tcpNoDelay = true
        raw.connect(InetSocketAddress(host, port), 8000)
        val ctx = SSLContext.getInstance("TLS")
        ctx.init(null, arrayOf(trustManager), java.security.SecureRandom())
        socket = ctx.socketFactory.createSocket(raw, host, port, true) as SSLSocket
        socket.startHandshake()
        output = socket.getOutputStream()
    }

    @Throws(IOException::class)
    fun send(packet: ByteArray) {
        output.write(packet)
        output.flush()
    }

    override fun close() {
        try { socket.close() } catch (_: Exception) {}
    }
}

/** Медиа-канал поверх RFCOMM (Bluetooth). */
class MediaRfcommChannel(private val output: OutputStream, private val input: InputStream?) : AutoCloseable {
    @Throws(IOException::class)
    fun send(packet: ByteArray) {
        output.write(packet)
        output.flush()
    }

    fun read(): ByteArray? {
        val ins = input ?: return null
        return try {
            val head = readFully(ins, 16)
            val plen = MediaPacketizer.getU16(head, 14)
            if (plen > Constants.MAX_CONTROL_MSG) throw IOException("bad plen")
            head + readFully(ins, plen)
        } catch (e: Exception) {
            null
        }
    }

    override fun close() {
        try { output.close() } catch (_: Exception) {}
        try { input?.close() } catch (_: Exception) {}
    }
}

/**
 * Discovery: рассылает "discover" по broadcast-адресам всех подсетей,
 * собирает ответы "announce" 1.5 с. Возвращает список ПК.
 */
object Discovery {

    data class PcAnnounce(val address: String, val name: String, val port: Int, val certFp: String)

    suspend fun discover(timeoutMs: Long = 1500): List<PcAnnounce> = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
        val results = LinkedHashMap<String, PcAnnounce>()
        val socket = DatagramSocket(null).apply {
            reuseAddress = true
            bind(InetSocketAddress(0))
            broadcast = true
        }
        try {
            val req = """{"t":"discover","proto":${Constants.PROTO_VERSION},"device":"${android.os.Build.MODEL}"}"""
                .toByteArray(Charsets.UTF_8)
            val targets = ArrayList<InetAddress>()
            for (iface in java.net.NetworkInterface.getNetworkInterfaces()) {
                if (!iface.isUp || iface.isLoopback) continue
                for (ia in iface.interfaceAddresses) {
                    val bcast = ia.broadcast ?: continue
                    targets.add(bcast)
                }
            }
            targets.add(InetAddress.getByName("255.255.255.255"))
            for (t in targets.toSet()) {
                try {
                    socket.send(DatagramPacket(req, req.size, t, Constants.DISCOVERY_PORT))
                } catch (_: Exception) {}
            }
            val deadline = System.currentTimeMillis() + timeoutMs
            val buf = ByteArray(4096)
            socket.soTimeout = 200
            while (System.currentTimeMillis() < deadline) {
                try {
                    val dp = DatagramPacket(buf, buf.size)
                    socket.receive(dp)
                    val json = parseJsonSafe(String(dp.data, dp.offset, dp.length, Charsets.UTF_8)) ?: continue
                    if (json.optString("t") == "announce" && json.optInt("proto") == Constants.PROTO_VERSION) {
                        val addr = dp.address.hostAddress ?: continue
                        results[addr] = PcAnnounce(
                            address = addr,
                            name = json.optString("name", "PC"),
                            port = json.optInt("port", Constants.CONTROL_PORT_DEFAULT),
                            certFp = json.optString("cert_fp", "")
                        )
                    }
                } catch (_: java.net.SocketTimeoutException) {
                } catch (_: Exception) {
                }
            }
        } catch (e: Exception) {
            AppLog.w("Discovery", "failed: ${e.message}")
        } finally {
            socket.close()
        }
        results.values.toList()
    }
}
