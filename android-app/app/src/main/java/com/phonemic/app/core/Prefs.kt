package com.phonemic.app.core

import android.content.Context
import android.content.SharedPreferences
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey

/** Настройки в EncryptedSharedPreferences (токен, пин сертификата, параметры потока). */
class Prefs private constructor(ctx: Context) {

    private val sp: SharedPreferences = run {
        val mk = MasterKey.Builder(ctx)
            .setKeyScheme(MasterKey.KeyScheme.AES256_GCM)
            .build()
        EncryptedSharedPreferences.create(
            ctx, "phonemic_prefs", mk,
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
            EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM
        )
    }

    var token: String
        get() = sp.getString(KEY_TOKEN, "") ?: ""
        set(v) = sp.edit().putString(KEY_TOKEN, v).apply()

    var certPin: String
        get() = sp.getString(KEY_PIN, "") ?: ""
        set(v) = sp.edit().putString(KEY_PIN, v).apply()

    fun resetPin() = sp.edit().remove(KEY_PIN).apply()

    var host: String
        get() = sp.getString(KEY_HOST, "") ?: ""
        set(v) = sp.edit().putString(KEY_HOST, v).apply()

    var port: Int
        get() = sp.getInt(KEY_PORT, Constants.CONTROL_PORT_DEFAULT)
        set(v) = sp.edit().putInt(KEY_PORT, v).apply()

    var transport: String
        get() = sp.getString(KEY_TRANSPORT, "wifi") ?: "wifi"
        set(v) = sp.edit().putString(KEY_TRANSPORT, v).apply()

    var codec: String
        get() = sp.getString(KEY_CODEC, "opus") ?: "opus"
        set(v) = sp.edit().putString(KEY_CODEC, v).apply()

    var frameMs: Int
        get() = sp.getInt(KEY_FRAME_MS, 20)
        set(v) = sp.edit().putInt(KEY_FRAME_MS, v).apply()

    var bitrateKbps: Int
        get() = sp.getInt(KEY_BITRATE, 48)
        set(v) = sp.edit().putInt(KEY_BITRATE, v).apply()

    var aec: Boolean
        get() = sp.getBoolean(KEY_AEC, true)
        set(v) = sp.edit().putBoolean(KEY_AEC, v).apply()

    var agc: Boolean
        get() = sp.getBoolean(KEY_AGC, false)
        set(v) = sp.edit().putBoolean(KEY_AGC, v).apply()

    var ns: Boolean
        get() = sp.getBoolean(KEY_NS, true)
        set(v) = sp.edit().putBoolean(KEY_NS, v).apply()

    var mediaMode: String
        get() = sp.getString(KEY_MEDIA_MODE, "auto") ?: "auto"
        set(v) = sp.edit().putString(KEY_MEDIA_MODE, v).apply()

    var keepScreenOn: Boolean
        get() = sp.getBoolean(KEY_KEEP_SCREEN, true)
        set(v) = sp.edit().putBoolean(KEY_KEEP_SCREEN, v).apply()

    var autoRestart: Boolean
        get() = sp.getBoolean(KEY_AUTO_RESTART, true)
        set(v) = sp.edit().putBoolean(KEY_AUTO_RESTART, v).apply()

    var wfdPassphrase: String
        get() = sp.getString(KEY_WFD_PASS, "") ?: ""
        set(v) = sp.edit().putString(KEY_WFD_PASS, v).apply()

    companion object {
        private const val KEY_TOKEN = "token"
        private const val KEY_PIN = "cert_pin"
        private const val KEY_HOST = "host"
        private const val KEY_PORT = "port"
        private const val KEY_TRANSPORT = "transport"
        private const val KEY_CODEC = "codec"
        private const val KEY_FRAME_MS = "frame_ms"
        private const val KEY_BITRATE = "bitrate_kbps"
        private const val KEY_AEC = "aec"
        private const val KEY_AGC = "agc"
        private const val KEY_NS = "ns"
        private const val KEY_MEDIA_MODE = "media_mode"
        private const val KEY_KEEP_SCREEN = "keep_screen"
        private const val KEY_AUTO_RESTART = "auto_restart"
        private const val KEY_WFD_PASS = "wfd_pass"

        @Volatile private var inst: Prefs? = null
        fun get(ctx: Context): Prefs = inst ?: synchronized(this) {
            inst ?: Prefs(ctx.applicationContext).also { inst = it }
        }
    }
}
