using System.Buffers.Binary;
using PhoneMic.Core.Crypto;
using PhoneMic.Core.Protocol;
using Xunit;

namespace PhoneMic.Core.Tests;

public class MediaPacketTests
{
    [Fact]
    public void Header_Roundtrip_BigEndian()
    {
        var h = new MediaPacketHeader
        {
            Magic = ProtocolConstants.MediaMagic,
            Version = 1,
            Flags = ProtocolConstants.FlagEncrypted,
            Seq = 0x01020304,
            Timestamp = 0xAABBCCDD,
            PayloadType = ProtocolConstants.PtypeOpus,
            Reserved = 0,
            PayloadLen = 0x0102,
        };
        var buf = new byte[16];
        h.Write(buf);
        Assert.Equal(0x50, buf[0]);
        Assert.Equal(0x4D, buf[1]);
        Assert.Equal(0x0102, BinaryPrimitives.ReadUInt16BigEndian(buf.AsSpan(14))); // PayloadLen BE

        Assert.True(MediaPacketHeader.TryParse(buf, out var parsed));
        Assert.Equal(h.Seq, parsed.Seq);
        Assert.Equal(h.Timestamp, parsed.Timestamp);
        Assert.Equal(h.PayloadLen, parsed.PayloadLen);
        Assert.Equal(h.PayloadType, parsed.PayloadType);
    }

    [Fact]
    public void Build_Parse_Roundtrip()
    {
        var payload = new byte[] { 1, 2, 3, 4, 5 };
        var packet = MediaPacket.Build(new MediaPacketHeader
        {
            Magic = ProtocolConstants.MediaMagic, Version = 1, Flags = 0,
            Seq = 7, Timestamp = 9, PayloadType = ProtocolConstants.PtypePcm,
            PayloadLen = (ushort)payload.Length,
        }, payload);
        Assert.Equal(21, packet.Length);
        Assert.True(MediaPacketHeader.TryParse(packet, out var h));
        Assert.Equal(7u, h.Seq);
    }
}

public class CryptoTests
{
    [Fact]
    public void Token_Format()
    {
        var t = TokenUtil.Generate();
        Assert.Equal(43, t.Length);
        Assert.DoesNotContain("=", t);
        var raw = TokenUtil.FromBase64Url(t);
        Assert.Equal(32, raw.Length);
    }

    [Fact]
    public void Hkdf_Rfc5869_TestVector1()
    {
        // RFC 5869 Appendix A, Test Case 1 (SHA-256)
        var ikm = Convert.FromHexString("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
        var salt = Convert.FromHexString("000102030405060708090a0b0c");
        var info = Convert.FromHexString("f0f1f2f3f4f5f6f7f8f9");
        var okm = HkdfDerive(ikm, salt, info, 42);
        var expected = Convert.FromHexString(
            "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
        Assert.Equal(expected, okm);
    }

    private static byte[] HkdfDerive(byte[] ikm, byte[] salt, byte[] info, int len)
        => System.Security.Cryptography.HKDF.DeriveKey(
            System.Security.Cryptography.HashAlgorithmName.SHA256, ikm, len, salt, info);

    [Fact]
    public void AesGcm_Roundtrip()
    {
        var tokenRaw = new byte[32];
        System.Security.Cryptography.RandomNumberGenerator.Fill(tokenRaw);
        var salt = new byte[32];
        System.Security.Cryptography.RandomNumberGenerator.Fill(salt);
        var keys = MediaCrypto.Derive(tokenRaw, salt);

        var enc = new DirectionalEncryptor(keys.MediaKey, keys.NoncePrefix.AsSpan(0, 4).ToArray());
        var dec = new DirectionalDecryptor(keys.MediaKey, keys.NoncePrefix.AsSpan(0, 4).ToArray());

        var plaintext = System.Text.Encoding.UTF8.GetBytes("hello phonemic media");
        var payload = enc.Next(plaintext);
        var decrypted = dec.Next(payload);
        Assert.Equal(plaintext, decrypted);
    }

    [Fact]
    public void AesGcm_Replay_Rejected()
    {
        var tokenRaw = new byte[32];
        System.Security.Cryptography.RandomNumberGenerator.Fill(tokenRaw);
        var salt = new byte[32];
        System.Security.Cryptography.RandomNumberGenerator.Fill(salt);
        var keys = MediaCrypto.Derive(tokenRaw, salt);
        var enc = new DirectionalEncryptor(keys.MediaKey, keys.NoncePrefix.AsSpan(0, 4).ToArray());
        var dec = new DirectionalDecryptor(keys.MediaKey, keys.NoncePrefix.AsSpan(0, 4).ToArray());
        var payload = enc.Next(new byte[] { 1 });
        dec.Next(payload);
        Assert.ThrowsAny<Exception>(() => dec.Next(payload)); // replay
    }

    [Fact]
    public void AuthMac_Formula()
    {
        var token = TokenUtil.Generate();
        var raw = TokenUtil.FromBase64Url(token);
        var nonce = new byte[16];
        System.Security.Cryptography.RandomNumberGenerator.Fill(nonce);
        var mac = TokenUtil.AuthMac(raw, nonce);
        Assert.Equal(32, mac.Length);

        // эквивалент формулы из протокола
        using var h = new System.Security.Cryptography.HMACSHA256(raw);
        var data = nonce.Concat(System.Text.Encoding.ASCII.GetBytes(ProtocolConstants.AuthContext)).ToArray();
        Assert.Equal(h.ComputeHash(data), mac);
    }
}
