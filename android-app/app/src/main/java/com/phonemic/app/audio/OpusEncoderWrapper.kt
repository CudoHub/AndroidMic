package com.phonemic.app.audio

import android.media.MediaCodec
import android.media.MediaFormat
import com.phonemic.app.core.AppLog
import com.phonemic.app.core.Constants
import java.nio.ByteBuffer

/**
 * Opus-кодировщик на MediaCodec ("audio/opus"). Вход — PCM16 48k mono кадры по 10 мс,
 * выход — Opus-кадры (обычно 20 мс). Синхронный режим, без внешних библиотек.
 * При недоступности кодировщика вызывающий код переключается на PCM-режим.
 */
class OpusEncoderWrapper(bitrateKbps: Int) {
    private val codec: MediaCodec
    private val bufferInfo = MediaCodec.BufferInfo()
    private var inputDone = false
    private var started = false

    val frameBytes: Int get() = Constants.PCM_CHUNK_BYTES

    init {
        val fmt = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, Constants.SAMPLE_RATE, Constants.CHANNELS)
        fmt.setInteger(MediaFormat.KEY_BIT_RATE, bitrateKbps * 1000)
        fmt.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 4096)
        codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS)
        codec.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        codec.start()
        started = true
        AppLog.i("OpusEnc", "started, bitrate=${bitrateKbps}kbps")
    }

    /**
     * Кодирует PCM-чанк. Возвращает список готовых Opus-кадров (может быть пуст —
     * кодировщик буферизует).
     */
    fun encode(pcm: ByteArray): List<ByteArray> {
        if (!started) return emptyList()
        val out = ArrayList<ByteArray>(2)
        // вход
        if (!inputDone) {
            val inIdx = codec.dequeueInputBuffer(0)
            if (inIdx >= 0) {
                val ib: ByteBuffer = codec.getInputBuffer(inIdx)!!
                ib.clear()
                ib.put(pcm)
                codec.queueInputBuffer(inIdx, 0, pcm.size, 0, 0)
            }
        }
        // выход
        var guard = 0
        while (guard++ < 8) {
            val outIdx = codec.dequeueOutputBuffer(bufferInfo, 0)
            if (outIdx >= 0) {
                if (bufferInfo.size > 0 && bufferInfo.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0) {
                    val ob: ByteBuffer = codec.getOutputBuffer(outIdx)!!
                    ob.position(bufferInfo.offset)
                    ob.limit(bufferInfo.offset + bufferInfo.size)
                    val frame = ByteArray(bufferInfo.size)
                    ob.get(frame)
                    out.add(frame)
                }
                codec.releaseOutputBuffer(outIdx, false)
                if (bufferInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) break
            } else if (outIdx == MediaCodec.INFO_TRY_AGAIN_LATER) {
                break
            } else {
                // INFO_OUTPUT_FORMAT_CHANGED и пр.
            }
        }
        return out
    }

    fun stop() {
        try {
            codec.stop()
            codec.release()
        } catch (e: Exception) {
            AppLog.w("OpusEnc", "stop: ${e.message}")
        }
        started = false
        AppLog.i("OpusEnc", "stopped")
    }
}
