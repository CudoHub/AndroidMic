namespace PhoneMic.Core.Protocol;

/// <summary>Константы протокола PhoneMic v1 — зеркало docs/PROTOCOL.md.</summary>
public static class ProtocolConstants
{
    public const int ProtoVersion = 1;
    public const int DiscoveryPort = 47820;
    public const int ControlPortDefault = 47821;
    public const int MediaPortDefault = 47822;
    public const ushort MediaMagic = 0x504D; // "PM"
    public const byte MediaVersion = 1;

    public const byte FlagEncrypted = 0x01;
    public const byte FlagMuted = 0x02;
    public const byte FlagEos = 0x04;

    public const byte PtypeOpus = 1;
    public const byte PtypePcm = 2;
    public const byte PtypeBtControl = 3;

    public const int SampleRate = 48000;
    public const int Channels = 1;
    public const int PcmChunkBytes = 1920;      // 10 мс
    public const int PcmChunkSamples = 960;

    public const int OpusPreskip = 312;

    public const string RfcommUuidCtl = "B2C4D6E8-F0A2-4C6E-8A0C-2E4A6C8E0B12";
    public const string RfcommUuidMedia = "B2C4D6E8-F0A2-4C6E-8A0C-2E4A6C8E0B13";

    public const string HkdfInfoMedia = "phonemic-media-v1";
    public const string HkdfInfoCtlBt = "phonemic-control-bt-v1";
    public const string HkdfInfoNonce = "phonemic-nonce-v1";
    public const string AuthContext = "phonemic-auth-v1";

    public const int MaxControlMsg = 65536;

    // пресеты джиттер-буфера: min / target / max, мс
    public static readonly (int Min, int Target, int Max) JitterUltra = (10, 30, 100);
    public static readonly (int Min, int Target, int Max) JitterBalance = (20, 40, 120);
    public static readonly (int Min, int Target, int Max) JitterStable = (40, 80, 250);
}
