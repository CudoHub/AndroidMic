package com.phonemic.app.net

import com.phonemic.app.core.AppLog
import com.phonemic.app.core.Constants
import com.phonemic.app.crypto.Crypto
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.EOFException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.security.SecureRandom
import java.security.cert.X509Certificate
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocket
import javax.net.ssl.TrustManager
import javax.net.ssl.X509TrustManager

/**
 * Результат TLS-соединения: сокет + реально предъявленный отпечаток сертификата.
 * При наличии сохранённого пина принимает только совпадающий сертификат.
 */
class PinnedTrustManager(private val expectedPin: String?) : X509TrustManager {
    @Volatile var presentedFingerprint: String? = null
        private set

    override fun checkClientTrusted(chain: Array<X509Certificate>, authType: String) {}

    override fun checkServerTrusted(chain: Array<X509Certificate>, authType: String) {
        if (chain.isEmpty()) throw java.security.cert.CertificateException("empty chain")
        val fp = Crypto.certFingerprintHex(chain[0])
        presentedFingerprint = fp
        if (!expectedPin.isNullOrEmpty() && !expectedPin.equals(fp, ignoreCase = true)) {
            throw java.security.cert.CertificateException("PIN_MISMATCH:$fp")
        }
    }

    override fun getAcceptedIssuers(): Array<X509Certificate> = arrayOf()
}

/**
 * Control-канал: TCP + TLS + JSON-фрейминг (4B BE length + UTF-8 JSON).
 * Реализует handshake §5.1 и обмен §5.2–5.3 из docs/PROTOCOL.md.
 */
class ControlChannel(
    private val host: String,
    private val port: Int,
    private val tokenRaw: ByteArray,
    private val expectedPin: String?
) {
    private var socket: Socket? = null
    private var ssl: SSLSocket? = null
    private var input: InputStream? = null
    private var output: OutputStream? = null
    private var trust: PinnedTrustManager? = null

    /** Отпечаток, предъявленный сервером при последнем соединении. */
    var presentedFingerprint: String? = null
        private set

    val isOpen: Boolean get() = ssl?.isConnected == true && ssl?.isClosed == false

    @Throws(Exception::class)
    fun connect() {
        val raw = Socket()
        raw.tcpNoDelay = true
        raw.connect(InetSocketAddress(host, port), 8000)
        socket = raw
        val tm = PinnedTrustManager(expectedPin)
        trust = tm
        val ctx = SSLContext.getInstance("TLS")
        ctx.init(null, arrayOf<TrustManager>(tm), SecureRandom())
        val s = ctx.socketFactory.createSocket(raw, host, port, true) as SSLSocket
        s.startHandshake()
        ssl = s
        input = s.getInputStream()
        output = s.getOutputStream()
        presentedFingerprint = tm.presentedFingerprint
        AppLog.i("Ctl", "TLS connected to $host:$port, fp=${presentedFingerprint?.take(16)}…")
    }

    // ---------- фрейминг ----------

    @Throws(Exception::class)
    fun send(json: JSONObject) {
        val body = json.toString().toByteArray(Charsets.UTF_8)
        if (body.size > Constants.MAX_CONTROL_MSG) throw IllegalStateException("msg too large")
        val head = ByteArray(4)
        MediaPacketizer.putU32(head, 0, body.size.toLong())
        synchronized(output!!) {
            output!!.write(head)
            output!!.write(body)
            output!!.flush()
        }
    }

    @Throws(Exception::class)
    fun receive(): JSONObject {
        val head = readFully(input!!, 4)
        val len = MediaPacketizer.getU32(head, 0).toInt()
        if (len <= 0 || len > Constants.MAX_CONTROL_MSG) throw EOFException("bad msg len $len")
        val body = readFully(input!!, len)
        return JSONObject(String(body, Charsets.UTF_8))
    }

    // ---------- handshake ----------

    /**
     * Полный handshake: hello → challenge → auth → ok/err.
     * @return JSONObject "ok" или кидает ProtocolException с кодом ошибки.
     */
    @Throws(Exception::class)
    fun handshake(
        appVersion: String,
        device: String,
        androidVer: String,
        transports: List<String>
    ): JSONObject {
        val caps = JSONObject()
            .put("opus", true).put("pcm", true)
            .put("aec", true).put("agc", true).put("ns", true)
            .put("bt", true)
        send(
            JSONObject()
                .put("t", "hello")
                .put("proto", Constants.PROTO_VERSION)
                .put("app", "PhoneMic-Android")
                .put("app_ver", appVersion)
                .put("device", device)
                .put("android", androidVer)
                .put("transports", org.json.JSONArray(transports))
                .put("caps", caps)
        )
        val challenge = receive()
        if (challenge.optString("t") != "challenge") throw ProtocolException("expected challenge")
        val nonce = Crypto.b64uDecode(challenge.getString("nonce"))
        send(
            JSONObject()
                .put("t", "auth")
                .put("mac", Crypto.b64uEncode(Crypto.authMac(tokenRaw, nonce)))
        )
        val resp = receive()
        return when (resp.optString("t")) {
            "ok" -> resp
            "err" -> throw ProtocolException(resp.optString("code", "internal"))
            else -> throw ProtocolException("unexpected ${resp.optString("t")}")
        }
    }

    @Throws(Exception::class)
    fun sendStartStream(
        codec: String, frameMs: Int, bitrateKbps: Int, mode: String,
        aec: Boolean, agc: Boolean, ns: Boolean, gain: Float
    ) = send(
        JSONObject()
            .put("t", "start_stream")
            .put("codec", codec)
            .put("frame_ms", frameMs)
            .put("bitrate_kbps", bitrateKbps)
            .put("mode", mode)
            .put("aec", aec).put("agc", agc).put("ns", ns)
            .put("gain", gain.toDouble())
    )

    @Throws(Exception::class)
    fun sendCommand(cmd: String, value: Any? = null) {
        val o = JSONObject().put("t", "cmd").put("cmd", cmd)
        when (value) {
            is Boolean -> o.put("value", value)
            is Double -> o.put("value", value)
            is Float -> o.put("value", value.toDouble())
            is Int -> o.put("value", value)
            else -> {}
        }
        send(o)
    }

    @Throws(Exception::class)
    fun sendPing(id: Long, tsClient: Long) =
        send(JSONObject().put("t", "ping").put("id", id).put("ts_client", tsClient))

    @Throws(Exception::class)
    fun sendBye(reason: String) {
        try { send(JSONObject().put("t", "bye").put("reason", reason)) } catch (_: Exception) {}
    }

    fun close() {
        try { ssl?.close() } catch (_: Exception) {}
        try { socket?.close() } catch (_: Exception) {}
        ssl = null; socket = null; input = null; output = null
    }

    companion object {
        private fun readFully(input: InputStream, n: Int): ByteArray {
            val buf = ByteArray(n)
            var off = 0
            while (off < n) {
                val r = input.read(buf, off, n - off)
                if (r < 0) throw EOFException("EOF in control channel")
                off += r
            }
            return buf
        }
    }
}

/** Ошибка уровня протокола с кодом из {"t":"err"}. */
class ProtocolException(val code: String) : Exception(code)

/** Чтение UDP-ответов discovery — вспомогательная функция. */
fun parseJsonSafe(s: String): JSONObject? = try { JSONObject(s) } catch (_: Exception) { null }
