package com.phonemic.app.core

/** Константы протокола PhoneMic v1 — зеркало docs/PROTOCOL.md. */
object Constants {
    const val PROTO_VERSION = 1
    const val DISCOVERY_PORT = 47820
    const val CONTROL_PORT_DEFAULT = 47821
    const val MEDIA_PORT_DEFAULT = 47822
    const val MAGIC = 0x504D
    const val VERSION = 1

    const val FLAG_ENCRYPTED = 0x01
    const val FLAG_MUTED = 0x02
    const val FLAG_EOS = 0x04

    const val PTYPE_OPUS = 1
    const val PTYPE_PCM = 2
    const val PTYPE_BT_CONTROL = 3

    const val SAMPLE_RATE = 48000
    const val CHANNELS = 1
    const val FRAME_MS_PCM = 10
    const val PCM_CHUNK_BYTES = 1920 // 10 мс, 16 бит, mono

    const val OPUS_PRESKIP = 312

    const val NAME_PREFIX = "PhoneMic"

    const val RFCOMM_UUID_CTL = "B2C4D6E8-F0A2-4C6E-8A0C-2E4A6C8E0B12"
    const val RFCOMM_UUID_MEDIA = "B2C4D6E8-F0A2-4C6E-8A0C-2E4A6C8E0B13"

    const val HKDF_INFO_MEDIA = "phonemic-media-v1"
    const val HKDF_INFO_CTLBT = "phonemic-control-bt-v1"
    const val HKDF_INFO_NONCE = "phonemic-nonce-v1"
    const val AUTH_CONTEXT = "phonemic-auth-v1"

    const val MAX_CONTROL_MSG = 65536
    const val PING_INTERVAL_MS = 1000L
    const val STATS_INTERVAL_MS = 2000L
    const val RECONNECT_MAX_MS = 8000L
}
