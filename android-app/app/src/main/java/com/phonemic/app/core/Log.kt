package com.phonemic.app.core

import android.util.Log
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** Лог с кольцевым буфером для экрана «Диагностика» и LogCat. Токен сюда не пишется никогда. */
object AppLog {
    private const val TAG = "PhoneMic"
    private val buf = ArrayDeque<String>(500)
    private val fmt = SimpleDateFormat("HH:mm:ss.SSS", Locale.US)

    @Synchronized
    fun i(where: String, msg: String) = log("I", where, msg)

    @Synchronized
    fun w(where: String, msg: String) = log("W", where, msg)

    @Synchronized
    fun e(where: String, msg: String, tr: Throwable? = null) = log("E", where, msg + (tr?.let { ": ${it.message}" } ?: ""))

    private fun log(level: String, where: String, msg: String) {
        val line = "${fmt.format(Date())} $level/$where: $msg"
        if (buf.size >= 500) buf.removeFirst()
        buf.addLast(line)
        when (level) {
            "E" -> Log.e(TAG, "[$where] $msg")
            "W" -> Log.w(TAG, "[$where] $msg")
            else -> Log.i(TAG, "[$where] $msg")
        }
    }

    @Synchronized
    fun dump(): List<String> = buf.toList()
}
