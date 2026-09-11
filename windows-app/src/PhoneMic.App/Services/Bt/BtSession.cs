using System.IO;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Text.Json;
using PhoneMic.Core.Audio;
using PhoneMic.Core.Crypto;
using PhoneMic.Core.Logging;
using PhoneMic.Core.Protocol;
using Windows.Devices.Enumeration;
using Windows.Devices.Bluetooth.Rfcomm;
using Windows.Networking.Sockets;
using Windows.Storage.Streams;

namespace PhoneMic.App.Services.Bt;

/// <summary>
/// Bluetooth-сессия: ПК подключается к RFCOMM-службам телефона (CTL+MEDIA).
/// Control — по §8.4 (открытый JSON до ok, затем AES-GCM ctlbt_key),
/// медиа — пакеты из MEDIA-сокета → декодер → джиттер → Pull960().
/// </summary>
public sealed class BtSession : IDisposable
{
    private StreamSocket? _ctl;
    private StreamSocket? _media;
    private CancellationTokenSource? _cts;

    public string DeviceName { get; private set; } = "";
    public bool Connected { get; private set; }
    public int LastRttMs { get; private set; }

    private JitterBuffer? _jitter;
    private OpusDecoderWrapper? _opus;
    private short[] _leftover = Array.Empty<short>();
    private uint _expectedSeq;
    private bool _seqStarted;
    public long PktTotal { get; private set; }
    public long PktLost { get; private set; }

    public event Action? Changed;

    /// <summary>Поиск спаренных телефонов с нашими RFCOMM-службами.</summary>
    public static async Task<IReadOnlyList<DeviceInformation>> FindPhonesAsync()
    {
        var serviceId = RfcommServiceId.FromUuid(Guid.Parse(ProtocolConstants.RfcommUuidCtl));
        var selector = RfcommDeviceService.GetDeviceSelector(serviceId);
        var devices = await DeviceInformation.FindAllAsync(selector);
        return devices.ToList();
    }

    public async Task<bool> ConnectAsync(string deviceId)
    {
        try
        {
            var serviceId = RfcommServiceId.FromUuid(Guid.Parse(ProtocolConstants.RfcommUuidCtl));
            var selector = RfcommDeviceService.GetDeviceSelector(serviceId);
            var devices = await DeviceInformation.FindAllAsync(selector);
            DeviceInformation? dev;
            if (!string.IsNullOrEmpty(deviceId))
                dev = devices.FirstOrDefault(d => d.Id == deviceId);
            else
                dev = devices.FirstOrDefault();
            if (dev == null)
            {
                AppLog.Warn("Bt", "RFCOMM CTL service not found on paired devices");
                return false;
            }
            DeviceName = dev.Name;

            var svc = await RfcommDeviceService.FromIdAsync(dev.Id);
            if (svc == null) { AppLog.Warn("Bt", "FromIdAsync null (разрешение?)"); return false; }

            _cts = new CancellationTokenSource();
            _ctl = new StreamSocket();
            await _ctl.ConnectAsync(svc.ConnectionHostName, svc.ConnectionServiceName);
            AppLog.Info("Bt", $"CTL connected: {DeviceName}");

            // media service
            var mediaId = RfcommServiceId.FromUuid(Guid.Parse(ProtocolConstants.RfcommUuidMedia));
            var mediaSelector = RfcommDeviceService.GetDeviceSelector(mediaId);
            var mediaDevices = await DeviceInformation.FindAllAsync(mediaSelector);
            var mediaDev = mediaDevices.FirstOrDefault(d => d.Name == DeviceName) ?? mediaDevices.FirstOrDefault();
            if (mediaDev == null) { AppLog.Warn("Bt", "MEDIA service not found"); return false; }
            var mediaSvc = await RfcommDeviceService.FromIdAsync(mediaDev.Id);
            if (mediaSvc == null) return false;
            _media = new StreamSocket();
            await _media.ConnectAsync(mediaSvc.ConnectionHostName, mediaSvc.ConnectionServiceName);
            AppLog.Info("Bt", "MEDIA connected");

            Connected = true;
            return true;
        }
        catch (Exception ex)
        {
            AppLog.Error("Bt", "connect failed", ex);
            Cleanup();
            return false;
        }
    }

    private ControlBtChannel? _ctlChannel;
    private byte[] _tokenRaw = Array.Empty<byte>();

    /// <summary>Handshake + start_stream + запуск приёма.</summary>
    public async Task<bool> StartSession(Settings settings)
    {
        if (_ctl == null || _media == null || !Connected) return false;

        var ctlStream = _ctl.InputStream.AsStreamForRead();
        var ctlOut = _ctl.OutputStream.AsStreamForWrite();
        _ctlChannel = new ControlBtChannel(ctlStream, ctlOut);
        _tokenRaw = TokenUtil.FromBase64Url(settings.Token);

        // handshake (открытый JSON, §8.4)
        await _ctlChannel.SendJson(new Dictionary<string, object?>
        {
            ["t"] = "hello", ["proto"] = ProtocolConstants.ProtoVersion,
            ["app"] = "PhoneMic-PC", ["app_ver"] = "1.0.0",
            ["device"] = Environment.MachineName, ["android"] = "",
            ["transports"] = new List<string> { "bt" },
            ["caps"] = new Dictionary<string, object?> { ["opus"] = true, ["pcm"] = true, ["bt"] = true }
        });
        var challenge = await _ctlChannel.ReceiveJson();
        if (challenge == null || challenge.Value.GetPropertySafe("t")?.ToString() != "challenge")
            return false;
        var nonce = TokenUtil.FromBase64Url(challenge.Value.GetPropertySafe("nonce")!.ToString()!);
        await _ctlChannel.SendJson(new Dictionary<string, object?>
        {
            ["t"] = "auth", ["mac"] = TokenUtil.ToBase64Url(TokenUtil.AuthMac(_tokenRaw, nonce))
        });
        var ok = await _ctlChannel.ReceiveJson();
        if (ok == null || ok.Value.GetPropertySafe("t")?.ToString() != "ok") return false;
        var mediaSalt = TokenUtil.FromBase64Url(ok.Value.GetPropertySafe("media_salt")!.ToString()!);
        _ctlChannel.EnableEncryption(_tokenRaw, mediaSalt);

        // джиттер + декодер (по BT только Opus)
        _jitter = new JitterBuffer(settings.Jitter);
        _opus = new OpusDecoderWrapper();

        // start_stream
        await _ctlChannel.SendJson(new Dictionary<string, object?>
        {
            ["t"] = "start_stream", ["codec"] = "opus", ["frame_ms"] = 20,
            ["bitrate_kbps"] = Math.Min(settings.BitrateKbps, 64), ["mode"] = "voice",
            ["aec"] = true, ["agc"] = false, ["ns"] = true, ["gain"] = 1.0
        });
        var started = await _ctlChannel.ReceiveJson();
        if (started == null || started.Value.GetPropertySafe("t")?.ToString() != "stream_started") return false;

        // reader: control (pong) + media (пакеты)
        _ = Task.Run(() => MediaLoop(_cts!.Token));
        _ = Task.Run(() => ControlLoop(_cts!.Token));
        _ = Task.Run(() => PingLoop(_cts!.Token));
        AppLog.Info("Bt", "session started");
        return true;
    }

    /// <summary>Старт аудиотракта (вызывается SessionController).</summary>
    public void StartAudio(Settings settings)
    {
        AppServices.Audio.Configure(settings.Jitter, "opus");
        AppServices.Audio.Volume = settings.Volume;
        AppServices.Audio.PcmSource = _ => Pull960();
        AppServices.Audio.Start();
        AppServices.Driver.SetActive(settings.Active);
    }

    private async Task MediaLoop(CancellationToken ct)
    {
        var stream = _media!.InputStream.AsStreamForRead(16384);
        while (!ct.IsCancellationRequested)
        {
            try
            {
                var head = await ReadExact(stream, MediaPacketHeader.Size, ct);
                if (head == null) break;
                if (!MediaPacketHeader.TryParse(head, out var h)) break;
                var payload = await ReadExact(stream, h.PayloadLen, ct);
                if (payload == null) break;
                if (h.PayloadType != ProtocolConstants.PtypeOpus) continue;
                if ((h.Flags & ProtocolConstants.FlagEncrypted) == 0) continue;

                var plain = _ctlChannel!.DecryptMedia(payload);
                PktTotal++;
                if (!_seqStarted) { _seqStarted = true; _expectedSeq = h.Seq; }
                else if (h.Seq > _expectedSeq)
                {
                    PktLost += h.Seq - _expectedSeq;
                    for (uint s = _expectedSeq; s < h.Seq; s++)
                    {
                        var plc = _opus!.DecodePlc(960);
                        if (plc.Length > 0) _jitter!.Push(s, plc);
                    }
                    _jitter!.NotifyGap();
                    _expectedSeq = h.Seq;
                }
                _expectedSeq = h.Seq + 1;

                var pcm = _opus!.Decode(plain, Math.Max(480, plain.Length * 4));
                if (pcm.Length > 0) _jitter!.Push(h.Seq, pcm);
                Changed?.Invoke();
            }
            catch (OperationCanceledException) { break; }
            catch (Exception ex)
            {
                AppLog.Warn("Bt", "media: " + ex.Message);
                break;
            }
        }
        Connected = false;
        AppLog.Warn("Bt", "media loop ended");
        Changed?.Invoke();
    }

    private async Task ControlLoop(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try
            {
                var msg = await _ctlChannel!.ReceiveJson();
                if (msg == null) break;
                if (msg.Value.GetPropertySafe("t")?.ToString() == "pong")
                {
                    var tsC = Convert.ToInt64(msg.Value.GetPropertySafe("ts_client") ?? 0);
                    if (tsC > 0) LastRttMs = (int)Math.Max(0, DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() - tsC);
                    Changed?.Invoke();
                }
            }
            catch { break; }
        }
    }

    private async Task PingLoop(CancellationToken ct)
    {
        long id = 1;
        while (!ct.IsCancellationRequested && Connected)
        {
            try
            {
                await Task.Delay(1000, ct);
                await _ctlChannel!.SendJson(new Dictionary<string, object?>
                {
                    ["t"] = "ping", ["id"] = id++, ["ts_client"] = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()
                });
            }
            catch { break; }
        }
    }

    public short[]? Pull960()
    {
        lock (this)
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

    public int BufferedMs => _jitter?.BufferedMs ?? 0;

    private static async Task<byte[]?> ReadExact(Stream s, int n, CancellationToken ct)
    {
        var buf = new byte[n];
        int off = 0;
        while (off < n)
        {
            int r = await s.ReadAsync(buf.AsMemory(off, n - off), ct);
            if (r <= 0) return null;
            off += r;
        }
        return buf;
    }

    private void Cleanup()
    {
        try { _ctl?.Dispose(); } catch { }
        try { _media?.Dispose(); } catch { }
        _ctl = null; _media = null;
        Connected = false;
    }

    public void Dispose()
    {
        try { _cts?.Cancel(); } catch { }
        Cleanup();
        _cts?.Dispose();
    }
}

/// <summary>Control-канал поверх RFCOMM (зеркало Android-класса ControlBtChannel).</summary>
public sealed class ControlBtChannel
{
    private readonly Stream _input;
    private readonly Stream _output;
    private DirectionalEncryptor? _enc;
    private DirectionalDecryptor? _dec;
    private uint _plainSeq;
    private readonly object _lock = new();

    public ControlBtChannel(Stream input, Stream output)
    {
        _input = input;
        _output = output;
    }

    public void EnableEncryption(byte[] tokenRaw, byte[] mediaSalt)
    {
        var keys = MediaCrypto.Derive(tokenRaw, mediaSalt);
        _enc = new DirectionalEncryptor(keys.CtlBtKey, keys.NoncePrefix.AsSpan(4, 4).ToArray());
        _dec = new DirectionalDecryptor(keys.CtlBtKey, keys.NoncePrefix.AsSpan(0, 4).ToArray());
    }

    public byte[] DecryptMedia(ReadOnlySpan<byte> payload) =>
        _dec != null ? _dec.Next(payload)
            : throw new InvalidOperationException("BT control not encrypted yet");

    public async Task SendJson(Dictionary<string, object?> msg)
    {
        var opts = new JsonSerializerOptions { PropertyNamingPolicy = System.Text.Json.JsonNamingPolicy.SnakeCaseLower };
        var body = JsonSerializer.SerializeToUtf8Bytes(msg, opts);
        byte[] packet;
        lock (_lock)
        {
            if (_enc != null)
            {
                var payload = _enc.Next(body);
                var h = new MediaPacketHeader
                {
                    Magic = ProtocolConstants.MediaMagic, Version = ProtocolConstants.MediaVersion,
                    Flags = ProtocolConstants.FlagEncrypted, Seq = 0, Timestamp = 0,
                    PayloadType = ProtocolConstants.PtypeBtControl,
                    PayloadLen = (ushort)payload.Length
                };
                packet = new byte[16 + payload.Length];
                h.Write(packet);
                payload.CopyTo(packet.AsSpan(16));
            }
            else
            {
                var h = new MediaPacketHeader
                {
                    Magic = ProtocolConstants.MediaMagic, Version = ProtocolConstants.MediaVersion,
                    Flags = 0, Seq = ++_plainSeq, Timestamp = 0,
                    PayloadType = ProtocolConstants.PtypeBtControl,
                    PayloadLen = (ushort)body.Length
                };
                packet = new byte[16 + body.Length];
                h.Write(packet);
                body.CopyTo(packet.AsSpan(16));
            }
            _output.Write(packet, 0, packet.Length);
            _output.Flush();
        }
    }

    public async Task<JsonElement?> ReceiveJson()
    {
        var head = await ReadExact(_input, 16, System.Threading.CancellationToken.None);
        if (head == null) return null;
        if (!MediaPacketHeader.TryParse(head, out var h)) return null;
        if (h.PayloadType != ProtocolConstants.PtypeBtControl) return null;
        var payload = await ReadExact(_input, h.PayloadLen, System.Threading.CancellationToken.None);
        if (payload == null) return null;
        byte[] plain;
        if ((h.Flags & ProtocolConstants.FlagEncrypted) != 0)
        {
            if (_dec == null) return null;
            plain = _dec.Next(payload);
        }
        else plain = payload;
        using var doc = JsonDocument.Parse(plain);
        return doc.RootElement.Clone();
    }

    private static async Task<byte[]?> ReadExact(Stream s, int n, CancellationToken ct)
    {
        var buf = new byte[n];
        int off = 0;
        while (off < n)
        {
            int r = await s.ReadAsync(buf.AsMemory(off, n - off), ct);
            if (r <= 0) return null;
            off += r;
        }
        return buf;
    }
}
