using System.Buffers.Binary;

namespace PhoneMic.Core.Protocol;

/// <summary>Заголовок медиа-пакета (16 байт, big-endian) — docs/PROTOCOL.md §6.</summary>
public readonly struct MediaPacketHeader
{
    public const int Size = 16;

    public ushort Magic { get; init; }
    public byte Version { get; init; }
    public byte Flags { get; init; }
    public uint Seq { get; init; }
    public uint Timestamp { get; init; }
    public byte PayloadType { get; init; }
    public byte Reserved { get; init; }
    public ushort PayloadLen { get; init; }

    public static bool TryParse(ReadOnlySpan<byte> span, out MediaPacketHeader h)
    {
        h = default;
        if (span.Length < Size) return false;
        ushort magic = BinaryPrimitives.ReadUInt16BigEndian(span);
        if (magic != ProtocolConstants.MediaMagic) return false;
        h = new MediaPacketHeader
        {
            Magic = magic,
            Version = span[2],
            Flags = span[3],
            Seq = BinaryPrimitives.ReadUInt32BigEndian(span.Slice(4)),
            Timestamp = BinaryPrimitives.ReadUInt32BigEndian(span.Slice(8)),
            PayloadType = span[12],
            Reserved = span[13],
            PayloadLen = BinaryPrimitives.ReadUInt16BigEndian(span.Slice(14)),
        };
        return h.Version == ProtocolConstants.MediaVersion;
    }

    public void Write(Span<byte> dst)
    {
        BinaryPrimitives.WriteUInt16BigEndian(dst, Magic);
        dst[2] = Version;
        dst[3] = Flags;
        BinaryPrimitives.WriteUInt32BigEndian(dst.Slice(4), Seq);
        BinaryPrimitives.WriteUInt32BigEndian(dst.Slice(8), Timestamp);
        dst[12] = PayloadType;
        dst[13] = Reserved;
        BinaryPrimitives.WriteUInt16BigEndian(dst.Slice(14), PayloadLen);
    }
}

/// <summary>Разобранный медиа-пакет с расшифрованной полезной нагрузкой.</summary>
public sealed class MediaPacket
{
    public MediaPacketHeader Header { get; init; }
    /// <summary>Открытый текст: Opus-кадр или PCM-чанк.</summary>
    public byte[] Plaintext { get; init; } = Array.Empty<byte>();
    public bool Muted => (Header.Flags & ProtocolConstants.FlagMuted) != 0;
    public bool Eos => (Header.Flags & ProtocolConstants.FlagEos) != 0;

    /// <summary>Собирает пакет (для тестов): header + payload.</summary>
    public static byte[] Build(MediaPacketHeader h, ReadOnlySpan<byte> payload)
    {
        var buf = new byte[MediaPacketHeader.Size + payload.Length];
        h.Write(buf);
        payload.CopyTo(buf.AsSpan(MediaPacketHeader.Size));
        return buf;
    }
}
