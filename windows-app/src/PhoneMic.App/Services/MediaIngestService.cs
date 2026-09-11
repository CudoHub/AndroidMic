using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using PhoneMic.Core.Audio;
using PhoneMic.Core.Crypto;
using PhoneMic.Core.Logging;
using PhoneMic.Core.Protocol;

namespace PhoneMic.App.Services;

/// <summary>
/// Приём медиа (UDP или TCP/TLS): расшифровка → декодирование → джиттер-буфер.
/// Разрывы seq заполняются PLC-кадрами декодера. Выдача — Pull960() по 10 мс.
/// </summary>
public sealed class MediaIngestService : IDisposable
{
    private UdpClient? _udp;
    private TcpListener? _tcpListener;
    private Task? _udpTask;
    private Task? _tcpTask;
    private CancellationTokenSource? _cts;

    private readonly object _lock = new();
    private JitterBuffer? _jitter;
    private OpusDecoderWrapper? _opus;
    private DirectionalDecryptor? _decryptor;
    private byte _ptype = ProtocolConstants.PtypeOpus;

    private short[] _leftover = Array.Empty<short>();
    private uint _expectedSeq;
    private bool _seqStarted;
    public long PktTotal { get; private set; }
    public long PktLost { get; private set; }
    public DateTime LastPacketUtc { get; private set; } = DateTime.MinValue;

    public bool UdpRunning { get; private set; }
    public bool TcpRunning { get; private set; }

    public event Action<long, long>? StatsChanged; // pktTotal, pktLost

    /// <summary>Настройка под сессию. Вызывается после handshake.</summary>
    public void Configure(byte[] tokenRaw, byte[] mediaSalt, string codec, (int Min, int Target, int Max) jitter)
    {
        lock (_lock)
        {
            var keys = MediaCrypto.Derive(tokenRaw, mediaSalt);
            _decryptor = new DirectionalDecryptor(keys.MediaKey, keys.NoncePrefix.AsSpan(0, 4).ToArray());
            _jitter = new JitterBuffer(jitter);
            _opus = codec == "opus" ? new OpusDecoderWrapper() : null;
            _ptype = codec == "opus" ? ProtocolConstants.PtypeOpus : ProtocolConstants.PtypePcm;
            _leftover = Array.Empty<short>();
            _expectedSeq = 0;
            _seqStarted = false;
            PktTotal = 0;
            PktLost = 0;
        }
    }

    public void StartUdp(int port)
    {
        StopUdp();
        _cts ??= new CancellationTokenSource();
        var ct = _cts.Token;
        _udp = new UdpClient(AddressFamily.InterNetwork);
        _udp.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
        _udp.Client.Bind(new IPEndPoint(IPAddress.Any, port));
        UdpRunning = true;
        _udpTask = Task.Run(async () =>
        {
            while (!ct.IsCancellationRequested)
            {
                try
                {
                    var res = await _udp.ReceiveAsync(ct);
                    IngestPacket(res.Buffer);
                }
                catch (OperationCanceledException) { break; }
                catch (Exception ex)
                {
                    AppLog.Warn("Media", "udp recv: " + ex.Message);
                }
            }
            UdpRunning = false;
        }, ct);
        AppLog.Info("Media", $"UDP listening :{port}");
    }

    public void StartTcp(int port, System.Security.Cryptography.X509Certificates.X509Certificate2 cert)
    {
        StopTcp();
        _cts ??= new CancellationTokenSource();
        var ct = _cts.Token;
        _tcpListener = new TcpListener(IPAddress.Any, port);
        _tcpListener.Server.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
        _tcpListener.Start();
        TcpRunning = true;
        _tcpTask = Task.Run(async () =>
        {
            while (!ct.IsCancellationRequested)
            {
                try
                {
                    using var client = await _tcpListener.AcceptTcpClientAsync(ct);
                    client.NoDelay = true;
                    using var ssl = new SslStream(client.GetStream(), false);
                    await ssl.AuthenticateAsServerAsync(new SslServerAuthenticationOptions
                    {
                        ServerCertificate = cert,
                        EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13
                    }, ct);
                    AppLog.Info("Media", $"TCP media client: {(client.Client.RemoteEndPoint as IPEndPoint)?.Address}");
                    // читаем пакеты: 16B header + payload
                    var buf = new byte[8192];
                    var acc = new MemoryStream();
                    while (!ct.IsCancellationRequested)
                    {
                        int n = await ssl.ReadAsync(buf, ct);
                        if (n <= 0) break;
                        acc.Write(buf, 0, n);
                        ProcessAccumulated(acc);
                    }
                }
                catch (OperationCanceledException) { break; }
                catch (Exception ex)
                {
                    AppLog.Warn("Media", "tcp media: " + ex.Message);
                }
            }
            TcpRunning = false;
        }, ct);
        AppLog.Info("Media", $"TCP listening :{port}");
    }

    private void ProcessAccumulated(MemoryStream acc)
    {
        var bytes = acc.ToArray();
        int pos = 0;
        while (bytes.Length - pos >= MediaPacketHeader.Size)
        {
            if (!MediaPacketHeader.TryParse(bytes.AsSpan(pos, MediaPacketHeader.Size), out var h))
            { acc.SetLength(0); return; }
            int total = MediaPacketHeader.Size + h.PayloadLen;
            if (bytes.Length - pos < total) break;
            IngestPacket(bytes.AsSpan(pos, total).ToArray());
            pos += total;
        }
        var rest = bytes[pos..];
        acc.SetLength(0);
        acc.Write(rest);
    }

    public void StopUdp()
    {
        try { _udp?.Close(); } catch { }
        _udp = null;
        UdpRunning = false;
    }

    public void StopTcp()
    {
        try { _tcpListener?.Stop(); } catch { }
        _tcpListener = null;
        TcpRunning = false;
    }

    /// <summary>Приём одного пакета (UDP или из TCP-потока).</summary>
    private void IngestPacket(byte[] packet)
    {
        LastPacketUtc = DateTime.UtcNow;
        if (packet.Length < MediaPacketHeader.Size) return;
        if (!MediaPacketHeader.TryParse(packet.AsSpan(0, MediaPacketHeader.Size), out var h)) return;
        var payload = packet.AsSpan(MediaPacketHeader.Size, h.PayloadLen);

        DirectionalDecryptor? dec;
        JitterBuffer? jb;
        OpusDecoderWrapper? opus;
        byte ptype;
        lock (_lock)
        {
            dec = _decryptor; jb = _jitter; opus = _opus; ptype = _ptype;
        }
        if (dec == null || jb == null) return;
        if (h.PayloadType != ptype) return;
        if ((h.Flags & ProtocolConstants.FlagEncrypted) == 0) return;

        byte[] plain;
        try
        {
            plain = dec.Next(payload);
        }
        catch (Exception ex)
        {
            AppLog.Warn("Media", $"decrypt: {ex.Message}");
            return;
        }

        lock (_lock)
        {
            PktTotal++;
            // потери по seq
            if (!_seqStarted)
            {
                _seqStarted = true;
                _expectedSeq = h.Seq;
            }
            else if (h.Seq > _expectedSeq)
            {
                long gap = h.Seq - _expectedSeq;
                PktLost += gap;
                // PLC для пропущенных пакетов
                if (opus != null)
                {
                    for (uint s = _expectedSeq; s < h.Seq; s++)
                    {
                        var plc = opus.DecodePlc(960);
                        if (plc.Length > 0) jb.Push(s, plc);
                    }
                }
                else
                {
                    for (uint s = _expectedSeq; s < h.Seq; s++)
                        jb.Push(s, new short[960]);
                }
                jb.NotifyGap();
                _expectedSeq = h.Seq;
            }
            _expectedSeq = h.Seq + 1;
        }

        // декодирование
        short[] pcm;
        if (opus != null && ptype == ProtocolConstants.PtypeOpus)
        {
            pcm = opus.Decode(plain, Math.Max(480, plain.Length * 4)); // 10/20 мс → сэмплы
        }
        else
        {
            // PCM16LE
            int n = plain.Length / 2;
            pcm = new short[n];
            Buffer.BlockCopy(plain, 0, pcm, 0, n * 2);
        }
        if (pcm.Length > 0) jb.Push(h.Seq, pcm);
        StatsChanged?.Invoke(PktTotal, PktLost);
    }

    /// <summary>Забрать очередной кадр 960 сэмплов (10 мс) с учётом остатков.</summary>
    public short[]? Pull960()
    {
        lock (_lock)
        {
            if (_jitter == null) return null;
            if (_leftover.Length >= 960)
            {
                var out1 = _leftover.Take(960).ToArray();
                _leftover = _leftover.Skip(960).ToArray();
                return out1;
            }
            var next = _jitter.Drain();
            if (next == null) return null;
            var combined = _leftover.Concat(next).ToArray();
            if (combined.Length < 960) { _leftover = combined; return null; }
            var res = combined.Take(960).ToArray();
            _leftover = combined.Skip(960).ToArray();
            return res;
        }
    }

    public int BufferedMs { get { lock (_lock) return _jitter?.BufferedMs ?? 0; } }
    public int TargetMs { get { lock (_lock) return _jitter?.TargetMs ?? 0; } }
    public long Underruns { get { lock (_lock) return _jitter?.Underruns ?? 0; } }

    public void Dispose()
    {
        try { _cts?.Cancel(); } catch { }
        StopUdp();
        StopTcp();
        _cts?.Dispose();
    }
}
