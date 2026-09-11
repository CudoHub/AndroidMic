using System.Net;
using System.Net.Sockets;
using System.Text.Json;
using PhoneMic.Core.Logging;
using PhoneMic.Core.Protocol;

namespace PhoneMic.App.Services;

/// <summary>
/// Discovery-ответчик (UDP 47820): отвечает на широковещательные запросы телефона.
/// </summary>
public sealed class DiscoveryService : IDisposable
{
    private UdpClient? _udp;
    private CancellationTokenSource? _cts;

    public bool Running { get; private set; }

    public void Start(int port)
    {
        Stop();
        try
        {
            _cts = new CancellationTokenSource();
            _udp = new UdpClient();
            _udp.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            _udp.Client.Bind(new IPEndPoint(IPAddress.Any, port));
            Running = true;
            var ct = _cts.Token;
            _ = Task.Run(async () =>
            {
                while (!ct.IsCancellationRequested)
                {
                    try
                    {
                        var res = await _udp.ReceiveAsync(ct);
                        var text = System.Text.Encoding.UTF8.GetString(res.Buffer);
                        JsonDocument? doc = null;
                        try { doc = JsonDocument.Parse(text); } catch { continue; }
                        using (doc)
                        {
                            if (doc!.RootElement.GetPropertySafe("t")?.ToString() != "discover") continue;
                            var resp = new Dictionary<string, object?>
                            {
                                ["t"] = "announce",
                                ["proto"] = ProtocolConstants.ProtoVersion,
                                ["name"] = Environment.MachineName,
                                ["port"] = SettingsService.Load().ControlPort,
                                ["cert_fp"] = CertService.Fingerprint(),
                                ["transports"] = new List<string> { "wifi", "usb", "wfd" },
                            };
                            var body = JsonSerializer.SerializeToUtf8Bytes(resp);
                            await _udp.SendAsync(body, body.Length, res.RemoteEndPoint);
                        }
                    }
                    catch (OperationCanceledException) { break; }
                    catch (Exception) { /* тихо */ }
                }
            }, ct);
            AppLog.Info("Discovery", $"answering on :{port}");
        }
        catch (Exception ex)
        {
            AppLog.Error("Discovery", "start failed", ex);
            Running = false;
        }
    }

    public void Stop()
    {
        try { _cts?.Cancel(); } catch { }
        try { _udp?.Close(); } catch { }
        _udp = null;
        Running = false;
    }

    public void Dispose() => Stop();
}
