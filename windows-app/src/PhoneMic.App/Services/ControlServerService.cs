using System.IO;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Text.Json;
using PhoneMic.Core.Crypto;
using PhoneMic.Core.Logging;
using PhoneMic.Core.Protocol;

namespace PhoneMic.App.Services;

/// <summary>
/// Control-сервер (TCP+TLS, порт 47821): handshake §5.1, команды §5.2, телеметрия §5.3.
/// Один активный клиент, остальные получают busy.
/// </summary>
public sealed class ControlServerService : IDisposable
{
    private TcpListener? _listener;
    private CancellationTokenSource? _cts;
    private Task? _acceptTask;

    public bool Running { get; private set; }
    public string ClientInfo { get; private set; } = "";

    // текущая сессия
    private byte[] _tokenRaw = Array.Empty<byte>();
    private volatile bool _streamActive;
    private DateTime _lastPingUtc = DateTime.MinValue;
    private int _lastRttMs;

    public bool SessionActive => _clientConnected;
    public bool StreamActive => _streamActive;
    private volatile bool _clientConnected;

    public int LastRttMs => _lastRttMs;
    public DateTime LastPingUtc => _lastPingUtc;

    public event Action<string>? ClientConnected;    // device name
    public event Action? ClientDisconnected;
    public event Action<string>? StreamStarted;      // codec
    public event Action? StreamStopped;
    public event Action<double>? VolumeReceived;     // set_volume с телефона
    public event Action<bool>? MuteReceived;         // set_mute с телефона
    public event Action? StopRequested;

    public void Start(int port)
    {
        Stop();
        _cts = new CancellationTokenSource();
        var ct = _cts.Token;
        _listener = new TcpListener(IPAddress.Any, port);
        _listener.Server.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
        _listener.Start();
        Running = true;
        _acceptTask = Task.Run(() => AcceptLoop(ct), ct);
        AppLog.Info("CtlSrv", $"listening :{port}");
    }

    public void Stop()
    {
        try { _listener?.Stop(); } catch { }
        _listener = null;
        Running = false;
        _clientConnected = false;
    }

    private async Task AcceptLoop(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            TcpClient client;
            try { client = await _listener!.AcceptTcpClientAsync(ct); }
            catch (OperationCanceledException) { break; }
            catch (SocketException) { break; }
            catch (ObjectDisposedException) { break; }

            var ep = client.Client.RemoteEndPoint as IPEndPoint;
            AppLog.Info("CtlSrv", $"client from {ep}");

            // один активный клиент
            if (_clientConnected)
            {
                try { SendErrOnFreshSocket(client, "busy"); } catch { }
                client.Dispose();
                continue;
            }
            _clientConnected = true;
            var epLocal = ep;
            _ = Task.Run(async () =>
            {
                try { await HandleClient(client, epLocal, ct); }
                finally
                {
                    _clientConnected = false;
                    ClientDisconnected?.Invoke();
                }
            }, ct);
        }
    }

    private static void SendErrOnFreshSocket(TcpClient client, string code)
    {
        // busy без TLS: просто закрываем (клиент увидит EOF при handshake)
        AppLog.Info("CtlSrv", $"rejected: {code}");
    }

    private async Task HandleClient(TcpClient client, IPEndPoint? ep, CancellationToken ct)
    {
        string device = "?";
        try
        {
            client.NoDelay = true;
            await using var net = client.GetStream();
            await using var ssl = new SslStream(net, false);
            var cert = CertService.GetOrCreate();
            await ssl.AuthenticateAsServerAsync(new SslServerAuthenticationOptions
            {
                ServerCertificate = cert,
                EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13
            }, ct);

            // ---- handshake ----
            var hello = await ReadJson(ssl, ct);
            long? proto = hello == null ? null : Convert.ToInt64(hello.Value.GetPropertySafe("proto") ?? -1);
            if (hello == null || hello.Value.GetPropertySafe("t")?.ToString() != "hello" ||
                proto != ProtocolConstants.ProtoVersion)
            {
                await Send(ssl, new Dictionary<string, object?> { ["t"] = "err", ["code"] = "wrong_proto" }, ct);
                return;
            }
            device = hello.Value.GetPropertySafe("device")?.ToString() ?? "?";
            ClientInfo = device;
            ClientConnected?.Invoke(device);

            var settings = SettingsService.Load();
            _tokenRaw = TokenUtil.FromBase64Url(settings.Token);

            var nonce = RandomNumberGenerator.GetBytes(16);
            await Send(ssl, new Dictionary<string, object?> { ["t"] = "challenge", ["nonce"] = TokenUtil.ToBase64Url(nonce) }, ct);

            var auth = await ReadJson(ssl, ct);
            if (auth == null || auth.Value.GetPropertySafe("t")?.ToString() != "auth")
            {
                await Send(ssl, new Dictionary<string, object?> { ["t"] = "err", ["code"] = "invalid_state" }, ct);
                return;
            }
            var macB64 = auth.Value.GetPropertySafe("mac")?.ToString() ?? "";
            byte[] expected, actual;
            try { expected = TokenUtil.AuthMac(_tokenRaw, nonce); actual = TokenUtil.FromBase64Url(macB64); }
            catch { await Send(ssl, new Dictionary<string, object?> { ["t"] = "err", ["code"] = "bad_token" }, ct); return; }
            if (!CryptographicOperations.FixedTimeEquals(expected, actual))
            {
                AppLog.Warn("CtlSrv", "auth failed (bad token)");
                await Send(ssl, new Dictionary<string, object?> { ["t"] = "err", ["code"] = "bad_token" }, ct);
                return;
            }

            // ---- ok ----
            var mediaSalt = RandomNumberGenerator.GetBytes(32);
            // режим медиа: настройка > детект
            string mediaMode = settings.MediaMode switch
            {
                "udp" => "udp",
                "tcp" => "tcp",
                _ => (ep?.Address.Equals(IPAddress.Loopback) ?? false) ? "tcp" : "udp"
            };
            var okMsg = new Dictionary<string, object?>
            {
                ["t"] = "ok",
                ["session"] = Guid.NewGuid().ToString(),
                ["media_port"] = settings.MediaPort,
                ["media_mode"] = mediaMode,
                ["cert_fp"] = TokenUtil.CertFingerprintHex(cert),
                ["media_salt"] = TokenUtil.ToBase64Url(mediaSalt),
            };
            await Send(ssl, okMsg, ct);
            AppLog.Info("CtlSrv", $"auth OK: {device}, media={mediaMode}");

            SessionStarted?.Invoke(device, _tokenRaw, mediaSalt, mediaMode, ep?.Address.ToString() ?? "");

            // ---- цикл сообщений ----
            while (!ct.IsCancellationRequested)
            {
                var msg = await ReadJson(ssl, ct);
                if (msg == null) break;
                string t = msg.Value.GetPropertySafe("t")?.ToString() ?? "";
                switch (t)
                {
                    case "start_stream":
                        var codec = msg.Value.GetPropertySafe("codec")?.ToString() ?? "opus";
                        _streamActive = true;
                        await Send(ssl, new Dictionary<string, object?> { ["t"] = "stream_started", ["media_mode"] = mediaMode }, ct);
                        StreamStarted?.Invoke(codec);
                        AppLog.Info("CtlSrv", $"stream started ({codec})");
                        break;

                    case "cmd":
                        var cmd = msg.Value.GetPropertySafe("cmd")?.ToString() ?? "";
                        switch (cmd)
                        {
                            case "set_volume":
                                double v = Convert.ToDouble(msg.Value.GetPropertySafe("value") ?? 1.0);
                                VolumeReceived?.Invoke(Math.Clamp(v, 0, 1.5));
                                break;
                            case "set_mute":
                                bool m = (msg.Value.GetPropertySafe("value")?.ToString() ?? "false") == "True" ||
                                          string.Equals(msg.Value.GetPropertySafe("value")?.ToString(), "true", StringComparison.OrdinalIgnoreCase);
                                MuteReceived?.Invoke(m);
                                break;
                            case "stop_stream":
                                _streamActive = false;
                                StreamStopped?.Invoke();
                                break;
                        }
                        break;

                    case "ping":
                        long id = Convert.ToInt64(msg.Value.GetPropertySafe("id") ?? 0);
                        long tsC = Convert.ToInt64(msg.Value.GetPropertySafe("ts_client") ?? 0);
                        _lastRttMs = tsC > 0 ? (int)Math.Max(0, (DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() - tsC)) : _lastRttMs;
                        _lastPingUtc = DateTime.UtcNow;
                        await Send(ssl, new Dictionary<string, object?>
                        {
                            ["t"] = "pong", ["id"] = id, ["ts_client"] = tsC,
                            ["ts_server"] = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()
                        }, ct);
                        break;

                    case "stats":
                        // телеметрия телефона (батарея и пр.) — в лог
                        break;

                    case "bye":
                        AppLog.Info("CtlSrv", $"bye: {msg.Value.GetPropertySafe("reason")}");
                        goto done;

                    default:
                        break;
                }
            }
        done:;
        }
        catch (Exception ex)
        {
            AppLog.Warn("CtlSrv", $"client {device}: {ex.Message}");
        }
        finally
        {
            if (_streamActive) { _streamActive = false; StreamStopped?.Invoke(); }
            ClientInfo = "";
            try { client.Dispose(); } catch { }
        }
    }

    public event Action<string, byte[], byte[], string, string>? SessionStarted;
    // (device, tokenRaw, mediaSalt, mediaMode, clientIp)

    // ---- JSON I/O ----

    private static async Task Send(SslStream ssl, Dictionary<string, object?> msg, CancellationToken ct)
    {
        var opts = new JsonSerializerOptions { PropertyNamingPolicy = System.Text.Json.JsonNamingPolicy.SnakeCaseLower };
        var body = JsonSerializer.SerializeToUtf8Bytes(msg, opts);
        var head = new byte[4];
        head[0] = (byte)(body.Length >> 24);
        head[1] = (byte)(body.Length >> 16);
        head[2] = (byte)(body.Length >> 8);
        head[3] = (byte)body.Length;
        await ssl.WriteAsync(head, ct);
        await ssl.WriteAsync(body, ct);
        await ssl.FlushAsync(ct);
    }

    private static async Task<JsonElement?> ReadJson(SslStream ssl, CancellationToken ct)
    {
        var head = await ReadExact(ssl, 4, ct);
        if (head == null) return null;
        int len = (head![0] << 24) | (head[1] << 16) | (head[2] << 8) | head[3];
        if (len <= 0 || len > ProtocolConstants.MaxControlMsg) return null;
        var body = await ReadExact(ssl, len, ct);
        if (body == null) return null;
        try
        {
            using var doc = JsonDocument.Parse(body);
            return doc.RootElement.Clone();
        }
        catch
        {
            return null;
        }
    }

    private static async Task<byte[]?> ReadExact(SslStream ssl, int n, CancellationToken ct)
    {
        var buf = new byte[n];
        int off = 0;
        while (off < n)
        {
            int r = await ssl.ReadAsync(buf.AsMemory(off, n - off), ct);
            if (r <= 0) return null;
            off += r;
        }
        return buf;
    }

    public void Dispose() => Stop();
}

/// <summary>Хелперы чтения полей JsonElement.</summary>
public static class JsonElementExt
{
    public static object? GetPropertySafe(this JsonElement el, string name)
    {
        if (el.ValueKind != JsonValueKind.Object) return null;
        if (!el.TryGetProperty(name, out var v)) return null;
        return v.ValueKind switch
        {
            JsonValueKind.String => v.GetString(),
            JsonValueKind.Number => v.TryGetInt64(out var l) ? l : v.GetDouble(),
            JsonValueKind.True => true,
            JsonValueKind.False => false,
            _ => v.ToString(),
        };
    }
}
