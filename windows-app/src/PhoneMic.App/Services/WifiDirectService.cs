using Windows.Devices.WiFiDirect;
using PhoneMic.Core.Logging;

namespace PhoneMic.App.Services;

/// <summary>
/// Wi-Fi Direct: ПК = Group Owner (публикация WiFiDirectAdvertisementPublisher).
/// Телефон подключается как P2P-клиент (DIRECT-…/WPA2, пароль в настройках),
/// получает адрес 192.168.137.x и сам подключается к control-порту.
/// </summary>
public sealed class WifiDirectService : IDisposable
{
    private WiFiDirectAdvertisementPublisher? _publisher;

    public bool Advertising { get; private set; }
    public string Status { get; private set; } = "";

    public event Action? Changed;

    public void Start()
    {
        if (Advertising) return;
        try
        {
            _publisher = new WiFiDirectAdvertisementPublisher();
            _publisher.Advertisement.ListenStateDiscoverability =
                WiFiDirectAdvertisementListenStateDiscoverability.Intensive;
            _publisher.StatusChanged += (p, args) =>
            {
                Advertising = args.Status == WiFiDirectAdvertisementPublisherStatus.Started;
                Status = Advertising ? "Публикация активна (Group Owner)"
                       : args.Status == WiFiDirectAdvertisementPublisherStatus.Aborted
                            ? "Ошибка Wi-Fi Direct: " + args.Error
                       : "Остановлено";
                AppLog.Info("WFD", $"{Status} ({args.Status})");
                Changed?.Invoke();
            };
            _publisher.Start();
            Advertising = true;
            Status = "Публикация активна (Group Owner)";
            AppLog.Info("WFD", "publisher started");
            Changed?.Invoke();
        }
        catch (Exception ex)
        {
            Status = "Wi-Fi Direct недоступен: " + ex.Message;
            AppLog.Error("WFD", "start failed", ex);
            Changed?.Invoke();
        }
    }

    public void Stop()
    {
        try { _publisher?.Stop(); } catch { }
        _publisher = null;
        Advertising = false;
        Status = "";
        Changed?.Invoke();
    }

    public void Dispose() => Stop();
}
