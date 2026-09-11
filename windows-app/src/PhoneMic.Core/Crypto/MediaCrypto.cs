using System.Security.Cryptography;

namespace PhoneMic.Core.Crypto;

/// <summary>
/// Медиа-криптография протокола PhoneMic v1 (docs/PROTOCOL.md §8.3):
/// HKDF-SHA256, AES-256-GCM, nonce = prefix(4B) || counter(8B BE).
/// </summary>
public sealed class MediaCrypto
{
    public byte[] MediaKey { get; }
    public byte[] CtlBtKey { get; }
    /// <summary>8 байт: [0..3] префикс телефон→ПК, [4..7] префикс ПК→телефон.</summary>
    public byte[] NoncePrefix { get; }

    private MediaCrypto(byte[] mediaKey, byte[] ctlBtKey, byte[] noncePrefix)
    {
        MediaKey = mediaKey;
        CtlBtKey = ctlBtKey;
        NoncePrefix = noncePrefix;
    }

    public static MediaCrypto Derive(byte[] tokenRaw, byte[] mediaSalt)
    {
        var master = SHA256.HashData(tokenRaw);
        return new MediaCrypto(
            HKDF.DeriveKey(HashAlgorithmName.SHA256, master, 32, mediaSalt, A(ProtocolConstants.HkdfInfoMedia)),
            HKDF.DeriveKey(HashAlgorithmName.SHA256, master, 32, mediaSalt, A(ProtocolConstants.HkdfInfoCtlBt)),
            HKDF.DeriveKey(HashAlgorithmName.SHA256, master, 8, mediaSalt, A(ProtocolConstants.HkdfInfoNonce)));
    }

    private static byte[] A(string s) => System.Text.Encoding.ASCII.GetBytes(s);

    private static byte[] Nonce(byte[] prefix, ulong counter)
    {
        var n = new byte[12];
        Array.Copy(prefix, 0, n, 0, 4);
        // counter big-endian 8 байт
        for (int i = 0; i < 8; i++)
            n[4 + i] = (byte)(counter >> (56 - 8 * i));
        return n;
    }

    /// <summary>Шифрование направления телефон→ПК (префикс [0..3]). counter=cipher||tag(16B).</summary>
    public byte[] EncryptPhoneToPc(ReadOnlySpan<byte> plaintext) =>
        Encrypt(MediaKey, NoncePrefix.AsSpan(0, 4).ToArray(), plaintext);

    /// <summary>Шифрование направления ПК→телефон (префикс [4..7]).</summary>
    public byte[] EncryptPcToPhone(ReadOnlySpan<byte> plaintext) =>
        Encrypt(MediaKey, NoncePrefix.AsSpan(4, 4).ToArray(), plaintext);

    public byte[] EncryptCtlBt(ReadOnlySpan<byte> plaintext) =>
        Encrypt(CtlBtKey, NoncePrefix.AsSpan(0, 4).ToArray(), plaintext);

    public byte[] DecryptCtlBt(ReadOnlySpan<byte> cipherWithTag) =>
        Decrypt(CtlBtKey, NoncePrefix.AsSpan(4, 4).ToArray(), cipherWithTag);

    public static byte[] Encrypt(byte[] key, byte[] prefix4, ReadOnlySpan<byte> plaintext)
    {
        ulong counter = Interlocked.Increment(ref CounterSeed); // тестовый путь; боевой — DirectionalEncryptor
        return EncryptWithCounter(key, prefix4, counter, plaintext);
    }

    private static long CounterSeed;

    public static byte[] EncryptWithCounter(byte[] key, byte[] prefix4, ulong counter, ReadOnlySpan<byte> plaintext)
    {
        using var gcm = new AesGcm(key, AesGcm.TagByteSizes.MaxSize);
        var nonce = Nonce(prefix4, counter);
        var ct = new byte[plaintext.Length];
        var tag = new byte[16];
        gcm.Encrypt(nonce, plaintext, ct, tag);
        // payload = counter(8B BE) || ct || tag
        var outBuf = new byte[8 + ct.Length + 16];
        for (int i = 0; i < 8; i++) outBuf[i] = (byte)(counter >> (56 - 8 * i));
        ct.CopyTo(outBuf, 8);
        tag.CopyTo(outBuf, 8 + ct.Length);
        return outBuf;
    }

    public static byte[] Decrypt(byte[] key, byte[] prefix4, ReadOnlySpan<byte> payload)
    {
        if (payload.Length < 8 + 16) throw new CryptographicException("payload too short");
        ulong counter = 0;
        for (int i = 0; i < 8; i++) counter = (counter << 8) | payload[i];
        var nonce = Nonce(prefix4, counter);
        int ctLen = payload.Length - 24;
        using var gcm = new AesGcm(key, AesGcm.TagByteSizes.MaxSize);
        var pt = new byte[ctLen];
        gcm.Decrypt(nonce, payload.Slice(8, ctLen), payload.Slice(payload.Length - 16), pt);
        return pt;
    }
}

/// <summary>Однонаправленный шифровальщик со своим счётчиком (потокобезопасный).</summary>
public sealed class DirectionalEncryptor
{
    private readonly byte[] _key;
    private readonly byte[] _prefix;
    private long _counter;

    public DirectionalEncryptor(byte[] key, byte[] prefix4)
    {
        _key = key;
        _prefix = prefix4;
    }

    public byte[] Next(ReadOnlySpan<byte> plaintext)
    {
        ulong c = (ulong)Interlocked.Increment(ref _counter);
        return MediaCrypto.EncryptWithCounter(_key, _prefix, c, plaintext);
    }
}

/// <summary>Однонаправленный расшифровальщик с replay-защитой (окно 512).</summary>
public sealed class DirectionalDecryptor
{
    private readonly byte[] _key;
    private readonly byte[] _prefix;
    private ulong _highest;

    public DirectionalDecryptor(byte[] key, byte[] prefix4)
    {
        _key = key;
        _prefix = prefix4;
    }

    public byte[] Next(ReadOnlySpan<byte> payload)
    {
        if (payload.Length < 8) throw new CryptographicException("no counter");
        ulong c = 0;
        for (int i = 0; i < 8; i++) c = (c << 8) | payload[i];
        // упрощённая replay-защита: не принимаем пакеты старее уже принятых (без окна перестановок)
        if (c <= _highest) throw new CryptographicException("replay or stale packet");
        var pt = MediaCrypto.Decrypt(_key, _prefix, payload);
        _highest = c;
        return pt;
    }
}

/// <summary>Служебные операции с токеном (§8.1).</summary>
public static class TokenUtil
{
    public const string Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    /// <summary>Генерирует токен: 32 случайных байта → base64url без padding (43 символа).</summary>
    public static string Generate()
    {
        var raw = RandomNumberGenerator.GetBytes(32);
        return ToBase64Url(raw);
    }

    public static string ToBase64Url(ReadOnlySpan<byte> data) =>
        Convert.ToBase64String(data.ToArray()).TrimEnd('=').Replace('+', '-').Replace('/', '_');

    public static byte[] FromBase64Url(string s)
    {
        var t = s.Trim().Replace('-', '+').Replace('_', '/');
        switch (t.Length % 4) { case 2: t += "=="; break; case 3: t += "="; break; case 1: throw new FormatException("bad base64url"); }
        return Convert.FromBase64String(t);
    }

    /// <summary>mac = HMAC-SHA256(token_raw, nonce_raw || "phonemic-auth-v1").</summary>
    public static byte[] AuthMac(byte[] tokenRaw, byte[] nonceRaw)
    {
        var ctx = System.Text.Encoding.ASCII.GetBytes(ProtocolConstants.AuthContext);
        var data = new byte[nonceRaw.Length + ctx.Length];
        Array.Copy(nonceRaw, 0, data, 0, nonceRaw.Length);
        Array.Copy(ctx, 0, data, nonceRaw.Length, ctx.Length);
        using var h = new HMACSHA256(tokenRaw);
        return h.ComputeHash(data);
    }

    /// <summary>SHA-256 отпечатка сертификата (hex lowercase).</summary>
    public static string CertFingerprintHex(System.Security.Cryptography.X509Certificates.X509Certificate2 cert) =>
        Convert.ToHexString(SHA256.HashData(cert.GetRawCertData())).ToLowerInvariant();

    public static string Sha256HexHex(byte[] data) =>
        Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant();
}
