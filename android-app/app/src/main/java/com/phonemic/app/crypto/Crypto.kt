package com.phonemic.app.crypto

import com.phonemic.app.core.Constants
import java.security.MessageDigest
import java.security.SecureRandom
import java.security.cert.Certificate
import java.util.Base64
import javax.crypto.Cipher
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/** Криптопримитивы протокола PhoneMic v1 (см. docs/PROTOCOL.md §8). Только javax.crypto. */
object Crypto {

    private val rnd = SecureRandom()

    // ---------- base64url без padding ----------

    fun b64uEncode(data: ByteArray): String =
        Base64.getUrlEncoder().withoutPadding().encodeToString(data)

    fun b64uDecode(s: String): ByteArray = Base64.getUrlDecoder().decode(s)

    // ---------- SHA-256 / HKDF (RFC 5869) ----------

    fun sha256(data: ByteArray): ByteArray =
        MessageDigest.getInstance("SHA-256").digest(data)

    fun hkdfSha256(ikm: ByteArray, salt: ByteArray, info: ByteArray, outLen: Int): ByteArray {
        // extract
        val prk = Mac.getInstance("HmacSHA256").apply {
            init(SecretKeySpec(if (salt.isEmpty()) ByteArray(32) else salt, "HmacSHA256"))
        }.doFinal(ikm)

        // expand
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(prk, "HmacSHA256"))
        val hashLen = 32
        val n = ((outLen + hashLen - 1) / hashLen)
        val okm = ByteArray(outLen)
        var t = ByteArray(0)
        var pos = 0
        for (i in 1..n) {
            mac.reset()
            mac.update(t)
            mac.update(info)
            mac.update(i.toByte())
            t = mac.doFinal()
            val toCopy = minOf(hashLen, outLen - pos)
            System.arraycopy(t, 0, okm, pos, toCopy)
            pos += toCopy
        }
        return okm
    }

    // ---------- HMAC ----------

    fun hmacSha256(key: ByteArray, data: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key, "HmacSHA256"))
        return mac.doFinal(data)
    }

    /** mac для auth: HMAC(token_raw, nonce_raw || "phonemic-auth-v1") */
    fun authMac(tokenRaw: ByteArray, nonceRaw: ByteArray): ByteArray =
        hmacSha256(tokenRaw, nonceRaw + Constants.AUTH_CONTEXT.toByteArray(Charsets.US_ASCII))

    // ---------- AES-256-GCM ----------

    fun aesGcmEncrypt(key: ByteArray, nonce: ByteArray, plaintext: ByteArray): ByteArray {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, nonce))
        return cipher.doFinal(plaintext) // ciphertext || tag(16B)
    }

    fun aesGcmDecrypt(key: ByteArray, nonce: ByteArray, ciphertextWithTag: ByteArray): ByteArray {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, nonce))
        return cipher.doFinal(ciphertextWithTag)
    }

    // ---------- Сертификаты ----------

    fun certFingerprintHex(cert: Certificate): String =
        sha256(cert.encoded).joinToString("") { "%02x".format(it) }

    // ---------- Прочее ----------

    fun randomBytes(n: Int): ByteArray = ByteArray(n).also { rnd.nextBytes(it) }

    /** Деривация медиа-ключей из токена и salt (§8.3). */
    class MediaKeys(mediaKey: ByteArray, ctlbtKey: ByteArray, noncePrefix: ByteArray) {
        val mediaKey = mediaKey.copyOf()
        val ctlbtKey = ctlbtKey.copyOf()
        val phonePrefix = noncePrefix.copyOfRange(0, 4)
        val pcPrefix = noncePrefix.copyOfRange(4, 8)

        companion object {
            fun derive(tokenRaw: ByteArray, mediaSalt: ByteArray): MediaKeys {
                val master = sha256(tokenRaw)
                return MediaKeys(
                    mediaKey = hkdfSha256(master, mediaSalt, Constants.HKDF_INFO_MEDIA.toByteArray(), 32),
                    ctlbtKey = hkdfSha256(master, mediaSalt, Constants.HKDF_INFO_CTLBT.toByteArray(), 32),
                    noncePrefix = hkdfSha256(master, mediaSalt, Constants.HKDF_INFO_NONCE.toByteArray(), 8)
                )
            }
        }
    }
}
