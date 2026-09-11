package com.phonemic.app.net

import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothServerSocket
import android.bluetooth.BluetoothSocket
import com.phonemic.app.core.AppLog
import com.phonemic.app.core.Constants
import com.phonemic.app.crypto.Crypto
import org.json.JSONObject
import java.io.EOFException
import java.io.InputStream
import java.io.OutputStream
import java.util.UUID

/**
 * Bluetooth: телефон = СЕРВЕР, ПК подключается.
 * Два secure RFCOMM-канала: control (...0B12) и media (...0B13).
 * Control-фрейминг — §8.4 PROTOCOL.md: до "ok" — открытый JSON (ptype=3, flags.encrypted=0),
 * после "ok" — шифрование ctlbt_key.
 */
class BluetoothServer {
    interface Listener {
        fun onCtlConnected(ctl: BluetoothSocket)
        fun onMediaConnected(media: BluetoothSocket)
        fun onError(msg: String)
    }

    private var ctlServer: BluetoothServerSocket? = null
    private var mediaServer: BluetoothServerSocket? = null
    @Volatile private var running = false

    fun start(adapter: BluetoothAdapter, listener: Listener) {
        running = true
        Thread({
            try {
                val ctlUuid = UUID.fromString(Constants.RFCOMM_UUID_CTL)
                val mediaUuid = UUID.fromString(Constants.RFCOMM_UUID_MEDIA)
                val ctl = adapter.listenUsingRfcommWithServiceRecord("PhoneMic-Ctl", ctlUuid)
                val media = adapter.listenUsingRfcommWithServiceRecord("PhoneMic-Media", mediaUuid)
                ctlServer = ctl
                mediaServer = media
                AppLog.i("BtSrv", "listening CTL+MEDIA")
                while (running) {
                    AppLog.i("BtSrv", "accept CTL…")
                    val ctlSock = ctl.accept() // блокирует до подключения ПК
                    if (!running) { try { ctlSock.close() } catch (_: Exception) {}; break }
                    AppLog.i("BtSrv", "CTL connected")
                    listener.onCtlConnected(ctlSock)
                    AppLog.i("BtSrv", "accept MEDIA…")
                    val mediaSock = media.accept()
                    if (!running) { try { mediaSock.close() } catch (_: Exception) {}; break }
                    AppLog.i("BtSrv", "MEDIA connected")
                    listener.onMediaConnected(mediaSock)
                }
            } catch (e: SecurityException) {
                listener.onError("нет разрешения BLUETOOTH_CONNECT")
            } catch (e: Exception) {
                if (running) listener.onError(e.message ?: "RFCOMM accept failed")
            }
        }, "phonemic-bt-server").start()
    }

    fun stop() {
        running = false
        try { ctlServer?.close() } catch (_: Exception) {}
        try { mediaServer?.close() } catch (_: Exception) {}
        ctlServer = null; mediaServer = null
    }
}

/**
 * Control-сессия поверх RFCOMM: те же JSON-сообщения, что и у ControlChannel.
 * До enableEncryption — открытый фрейминг; после — AES-GCM ctlbt_key (§8.4).
 */
class ControlBtChannel(
    private val input: InputStream,
    private val output: OutputStream
) {
    private var framer: BtControlFramer? = null
    private var decryptor: MediaDecryptor? = null

    @Synchronized
    @Throws(Exception::class)
    fun send(json: JSONObject) {
        val f = framer
        val packet = if (f != null) f.frame(json.toString())
                     else plainFrame(json.toString().toByteArray(Charsets.UTF_8))
        output.write(packet)
        output.flush()
    }

    @Throws(Exception::class)
    fun receive(): JSONObject {
        val head = readFully(input, 16)
        val flags = head[3].toInt() and 0xFF
        val ptype = head[12].toInt() and 0xFF
        val plen = MediaPacketizer.getU16(head, 14)
        if (ptype != Constants.PTYPE_BT_CONTROL) throw EOFException("unexpected ptype $ptype")
        if (plen <= 0 || plen > Constants.MAX_CONTROL_MSG) throw EOFException("bad plen $plen")
        val payload = readFully(input, plen)
        val plain = if (flags and Constants.FLAG_ENCRYPTED != 0) {
            val d = decryptor ?: throw EOFException("encrypted msg before keys")
            d.decrypt(payload.copyOfRange(0, 8), payload.copyOfRange(8, payload.size))
        } else {
            payload
        }
        return JSONObject(String(plain, Charsets.UTF_8))
    }

    private fun plainFrame(body: ByteArray): ByteArray =
        MediaPacketizer.packet(0, nextPlainSeq(), 0, Constants.PTYPE_BT_CONTROL, body)

    private var plainSeq = 0L
    @Synchronized private fun nextPlainSeq(): Long = ++plainSeq

    /** Переключение фреймера на шифрованный режим с ctlbt_key. */
    @Synchronized
    fun enableEncryption(tokenRaw: ByteArray, mediaSalt: ByteArray) {
        val keys = Crypto.MediaKeys.derive(tokenRaw, mediaSalt)
        framer = BtControlFramer(keys)
        decryptor = MediaDecryptor(keys)
    }
}
