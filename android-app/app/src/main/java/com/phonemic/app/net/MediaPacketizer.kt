package com.phonemic.app.net

import com.phonemic.app.core.Constants
import com.phonemic.app.crypto.Crypto
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.nio.ByteBuffer

/**
 * Медиа-пакет PhoneMic (docs/PROTOCOL.md §6): 16-байтный заголовок big-endian +
 * payload = counter(8B) + ciphertext + tag(16B).
 */
object MediaPacketizer {

    fun buildHeader(flags: Int, seq: Long, tsSamples: Long, ptype: Int, payloadLen: Int): ByteArray {
        val h = ByteArray(16)
        h[0] = (Constants.MAGIC shr 8).toByte()
        h[1] = (Constants.MAGIC and 0xFF).toByte()
        h[2] = Constants.VERSION.toByte()
        h[3] = flags.toByte()
        putU32(h, 4, seq)
        putU32(h, 8, tsSamples)
        h[12] = ptype.toByte()
        h[13] = 0
        putU16(h, 14, payloadLen)
        return h
    }

    fun putU16(b: ByteArray, off: Int, v: Int) {
        b[off] = ((v shr 8) and 0xFF).toByte()
        b[off + 1] = (v and 0xFF).toByte()
    }

    fun putU32(b: ByteArray, off: Int, v: Long) {
        b[off] = ((v shr 24) and 0xFF).toByte()
        b[off + 1] = ((v shr 16) and 0xFF).toByte()
        b[off + 2] = ((v shr 8) and 0xFF).toByte()
        b[off + 3] = (v and 0xFF).toByte()
    }

    fun getU16(b: ByteArray, off: Int): Int = ((b[off].toInt() and 0xFF) shl 8) or (b[off + 1].toInt() and 0xFF)
    fun getU32(b: ByteArray, off: Int): Long {
        var v = 0L
        for (i in 0 until 4) v = (v shl 8) or (b[off + i].toLong() and 0xFF)
        return v
    }

    /** Полный пакет (заголовок + payload). */
    fun packet(flags: Int, seq: Long, tsSamples: Long, ptype: Int, payload: ByteArray): ByteArray {
        val out = ByteArray(16 + payload.size)
        System.arraycopy(buildHeader(flags, seq, tsSamples, ptype, payload.size), 0, out, 0, 16)
        System.arraycopy(payload, 0, out, 16, payload.size)
        return out
    }
}

/**
 * Шифровальщик медиа-потока в направлении телефон → ПК.
 * nonce = phonePrefix(4B) || counter(8B BE); счётчик начинается с 1.
 */
class MediaEncryptor(keys: Crypto.MediaKeys) {
    private val key = keys.mediaKey
    private val prefix = keys.phonePrefix
    private val counter = java.util.concurrent.atomic.AtomicLong(1)

    /** Возвращает counter(8B) || ciphertext||tag. */
    fun encrypt(plaintext: ByteArray): ByteArray {
        val c = counter.getAndIncrement()
        val nonce = ByteArray(12)
        System.arraycopy(prefix, 0, nonce, 0, 4)
        val cb = ByteArray(8)
        MediaPacketizer.putU32(cb, 0, (c ushr 32) and 0xFFFFFFFFL)
        MediaPacketizer.putU32(cb, 4, c and 0xFFFFFFFFL)
        System.arraycopy(cb, 0, nonce, 4, 8)
        val enc = Crypto.aesGcmEncrypt(key, nonce, plaintext)
        val out = ByteArray(8 + enc.size)
        System.arraycopy(cb, 0, out, 0, 8)
        System.arraycopy(enc, 0, out, 8, enc.size)
        return out
    }
}

/** Дешифровальщик направления ПК → телефон (для BT control, ptype=3, ответы сервера). */
class MediaDecryptor(keys: Crypto.MediaKeys) {
    private val key = keys.ctlbtKey
    private val prefix = keys.pcPrefix
    private var lastCounter = 0L

    @Synchronized
    fun decrypt(counterBytes: ByteArray, cipherWithTag: ByteArray): ByteArray {
        var c = 0L
        for (b in counterBytes) c = (c shl 8) or (b.toLong() and 0xFF)
        if (c <= lastCounter) throw IOException("replay/out-of-order control packet")
        lastCounter = c
        val nonce = ByteArray(12)
        System.arraycopy(prefix, 0, nonce, 0, 4)
        System.arraycopy(counterBytes, 0, nonce, 4, 8)
        return Crypto.aesGcmDecrypt(key, nonce, cipherWithTag)
    }
}

/** Построитель зашифрованных медиа-пакетов с последовательностями и таймкодами. */
class MediaStreamPacker(keys: Crypto.MediaKeys, private val ptype: Int) {
    private val enc = MediaEncryptor(keys)
    private val seq = java.util.concurrent.atomic.AtomicLong(1)
    @Volatile var muted = false
    private var tsSamples = 0L

    fun next(frame: ByteArray, frameSamples: Long, eos: Boolean = false): ByteArray {
        var flags = Constants.FLAG_ENCRYPTED
        if (muted) flags = flags or Constants.FLAG_MUTED
        if (eos) flags = flags or Constants.FLAG_EOS
        val s = seq.getAndIncrement()
        val ts = tsSamples
        tsSamples += frameSamples
        val payload = enc.encrypt(frame)
        return MediaPacketizer.packet(flags, s, ts, ptype, payload)
    }
}

/** Чтение ровно n байт из потока (TCP/RFCOMM). */
@Throws(IOException::class)
fun readFully(input: java.io.InputStream, n: Int): ByteArray {
    val buf = ByteArray(n)
    var off = 0
    while (off < n) {
        val r = input.read(buf, off, n - off)
        if (r < 0) throw IOException("EOF")
        off += r
    }
    return buf
}

/** Построитель зашифрованных control-фреймов BT-канала (шифрование ctlbt_key, §8.4). */
class BtControlFramer(keys: Crypto.MediaKeys) {
    private val enc = object {
        val key = keys.ctlbtKey
        val prefix = keys.phonePrefix
        val counter = java.util.concurrent.atomic.AtomicLong(1)
        fun encrypt(plaintext: ByteArray): ByteArray {
            val c = counter.getAndIncrement()
            val nonce = ByteArray(12)
            System.arraycopy(prefix, 0, nonce, 0, 4)
            val cb = ByteArray(8)
            MediaPacketizer.putU32(cb, 0, (c ushr 32) and 0xFFFFFFFFL)
            MediaPacketizer.putU32(cb, 4, c and 0xFFFFFFFFL)
            System.arraycopy(cb, 0, nonce, 4, 8)
            val e = Crypto.aesGcmEncrypt(key, nonce, plaintext)
            val out = ByteArray(8 + e.size)
            System.arraycopy(cb, 0, out, 0, 8)
            System.arraycopy(e, 0, out, 8, e.size)
            return out
        }
    }
    private val seq = java.util.concurrent.atomic.AtomicLong(1)

    fun frame(json: String): ByteArray {
        val s = seq.getAndIncrement()
        val payload = enc.encrypt(json.toByteArray(Charsets.UTF_8))
        return MediaPacketizer.packet(Constants.FLAG_ENCRYPTED, s, 0, Constants.PTYPE_BT_CONTROL, payload)
    }
}
