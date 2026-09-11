package com.phonemic.app.audio

import android.annotation.SuppressLint
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.media.audiofx.AcousticEchoCanceler
import android.media.audiofx.AutomaticGainControl
import android.media.audiofx.NoiseSuppressor
import com.phonemic.app.core.AppLog
import com.phonemic.app.core.Constants
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.channels.Channel
import kotlin.math.sqrt

/**
 * Захват микрофона: 48 кГц / mono / PCM16, кадры по 10 мс (960 сэмплов).
 * Применяет AEC/AGC/NS при наличии, локальный gain и mute, считает RMS для индикатора.
 */
class CaptureEngine(
    private val useAec: Boolean,
    private val useAgc: Boolean,
    private val useNs: Boolean
) {
    private var record: AudioRecord? = null
    private var aec: AcousticEchoCanceler? = null
    private var agc: AutomaticGainControl? = null
    private var ns: NoiseSuppressor? = null

    @Volatile var gain: Float = 1.0f
    @Volatile var muted: Boolean = false
        private set

    @Volatile var rmsLevel: Float = 0f // 0..1 сглаженный
        private set

    /** Канал кадров по 10 мс (1920 байт PCM16LE mono 48k). */
    val frames = Channel<ByteArray>(capacity = 16, onBufferOverflow = BufferOverflow.DROP_OLDEST)

    @SuppressLint("MissingPermission") // разрешение проверяется до старта
    fun start(): Boolean {
        if (record != null) return true
        val minBuf = AudioRecord.getMinBufferSize(
            Constants.SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT
        )
        if (minBuf <= 0) {
            AppLog.e("Capture", "getMinBufferSize=$minBuf")
            return false
        }
        // VOICE_COMMUNICATION включает аппаратную обработку (AEC), фолбэк на MIC
        val source = if (useAec) MediaRecorder.AudioSource.VOICE_COMMUNICATION
                     else MediaRecorder.AudioSource.MIC
        val rec = try {
            AudioRecord(
                source, Constants.SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT, maxOf(minBuf * 2, 4096)
            )
        } catch (e: Exception) {
            AppLog.e("Capture", "AudioRecord create failed", e)
            return false
        }
        if (rec.state != AudioRecord.STATE_INITIALIZED) {
            AppLog.e("Capture", "AudioRecord not initialized")
            rec.release()
            return false
        }
        try {
            if (useAec) aec = AcousticEchoCanceler.create(rec.audioSessionId)?.apply { enabled = true }
            if (useAgc) agc = AutomaticGainControl.create(rec.audioSessionId)?.apply { enabled = true }
            if (useNs) ns = NoiseSuppressor.create(rec.audioSessionId)?.apply { enabled = true }
        } catch (e: Exception) {
            AppLog.w("Capture", "audiofx unavailable: ${e.message}")
        }
        rec.startRecording()
        record = rec
        AppLog.i("Capture", "started, session=${rec.audioSessionId}")
        return true
    }

    /** Читает один кадр 10 мс; применяет gain/mute; кладёт в канал. Вызывать из цикла захвата. */
    fun readFrame(): ByteArray? {
        val rec = record ?: return null
        val buf = ByteArray(Constants.PCM_CHUNK_BYTES)
        var off = 0
        while (off < buf.size) {
            val n = rec.read(buf, off, buf.size - off)
            if (n < 0) return null
            if (n == 0) continue
            off += n
        }
        // RMS до обработки (для честного индикатора)
        var acc = 0.0
        for (i in 0 until buf.size step 2) {
            val s = (buf[i].toInt() and 0xFF) or (buf[i + 1].toInt() shl 8)
            val v = s.toShort().toInt()
            acc += (v * v).toDouble()
        }
        val rms = sqrt(acc / (buf.size / 2)) / 32768.0
        rmsLevel = (rmsLevel * 0.7f + rms.toFloat() * 0.3f).coerceIn(0f, 1f)

        if (muted) {
            java.util.Arrays.fill(buf, 0)
        } else if (gain != 1.0f) {
            for (i in 0 until buf.size step 2) {
                val s = (((buf[i].toInt() and 0xFF) or (buf[i + 1].toInt() shl 8)).toShort().toInt()) * gain
                val clamped = s.toInt().coerceIn(Short.MIN_VALUE.toInt(), Short.MAX_VALUE.toInt())
                buf[i] = (clamped and 0xFF).toByte()
                buf[i + 1] = ((clamped shr 8) and 0xFF).toByte()
            }
        }
        frames.trySend(buf)
        return buf
    }

    fun setMuted(m: Boolean) {
        muted = m
        AppLog.i("Capture", "muted=$m")
    }

    fun stop() {
        try {
            record?.stop()
        } catch (_: Exception) {}
        try {
            record?.release()
        } catch (_: Exception) {}
        record = null
        aec?.release(); aec = null
        agc?.release(); agc = null
        ns?.release(); ns = null
        rmsLevel = 0f
        AppLog.i("Capture", "stopped")
    }
}
