using System.Net;
using PhoneMic.Core.Logging;
using PhoneMic.Core.Protocol;

namespace PhoneMic.App.Services;

/// <summary>Глобальный доступ к сервисам (DI-lite).</summary>
public static class AppServices
{
    public static Settings Settings = null!;
    public static DriverService Driver = null!;
    public static AudioPipelineService Audio = null!;
    public static ControlServerService ControlServer = null!;
    public static MediaIngestService Media = null!;
    public static SessionController Session = null!;
    public static WifiDirectService WifiDirect = null!;
    public static UsbAdbService Adb = null!;
    public static DiscoveryService Discovery = null!;

    public static void Init()
    {
        Settings = SettingsService.Load();
        Driver = new DriverService();
        Audio = new AudioPipelineService();
        ControlServer = new ControlServerService();
        Media = new MediaIngestService();
        Session = new SessionController();
        WifiDirect = new WifiDirectService();
        Adb = new UsbAdbService();
        Discovery = new DiscoveryService();

        Session.Initialize();
    }
}

/// <summary>
/// Фасад сессии: связывает ControlServer, MediaIngest, AudioPipeline, Driver, Bluetooth.
/// Полностью управляет жизненным циклом: событие от сети → настроить тракт → подать в драйвер.
/// </summary>
public sealed class SessionController : IDisposable
{
    public string PhoneDevice { get; private set; } = "";
    public string TransportKind { get; private set; } = ""; // wifi/usb/wfd/bt
    public string Codec { get; private set; } = "";

    private readonly object _lock = new();
    private Bt.BtSession? _btSession;
    private CancellationTokenSource? _statsCts;

    public event Action? Changed;

    // статистика для UI
    public long PktTotal => AppServices.Media.PktTotal;
    public long PktLost => AppServices.Media.PktLost;
    public double LossPct => PktTotal > 0 ? 100.0 * PktLost / Math.Max(PktTotal, 1) : 0;
    public int BufferedMs => AppServices.Media.BufferedMs;
    public bool DriverOk => AppServices.Driver.IsPresent;

    public void Initialize()
    {
        var cs = AppServices.ControlServer;
        cs.SessionStarted += OnSessionStarted;
        cs.ClientConnected += _ => { Changed?.Invoke(); };
        cs.ClientDisconnected += OnClientDisconnected;
        cs.StreamStarted += OnStreamStarted;
        cs.StreamStopped += OnStreamStopped;
        cs.VolumeReceived += v =>
        {
            AppServices.Settings.Volume = v;
            AppServices.Audio.Volume = v;
            SettingsService.Save(AppServices.Settings);
            Changed?.Invoke();
        };
        cs.MuteReceived += m =>
        {
            // мьют НА ТЕЛЕФОНЕ — приёмник тоже глушим (флаг уже в пакетах), дублируем локально
            AppServices.Audio.Mute = m;
            Changed?.Invoke();
        };
        cs.StopRequested += () => { };

        // подача PCM в аудиотракт
        AppServices.Audio.PcmSource = _ => AppServices.Media.Pull960();
    }

    private void OnSessionStarted(string device, byte[] tokenRaw, byte[] mediaSalt, string mediaMode, string clientIp)
    {
        lock (_lock)
        {
            PhoneDevice = device;
            TransportKind = clientIp == "127.0.0.1" ? "usb-adb" : ClassifyIp(clientIp);
            var s = AppServices.Settings;
            var jitter = s.Jitter;
            var codec = s.Codec;
            Codec = codec;
            AppServices.Media.Configure(tokenRaw, mediaSalt, codec, jitter);
            if (mediaMode == "udp") AppServices.Media.StartUdp(s.MediaPort);
            else AppServices.Media.StartTcp(s.MediaPort, CertService.GetOrCreate());
        }
        Changed?.Invoke();
    }

    private static string ClassifyIp(string ip)
    {
        if (IPAddress.TryParse(ip, out var a))
        {
            if (a.Equals(IPAddress.Loopback)) return "usb-adb";
            var bytes = a.GetAddressBytes();
            if (bytes.Length == 4 && bytes[0] == 192 && bytes[1] == 168 && bytes[2] == 137) return "wfd";
        }
        return "wifi";
    }

    private void OnStreamStarted(string codec)
    {
        AppServices.Audio.Configure(AppServices.Settings.Jitter, Codec);
        AppServices.Audio.Volume = AppServices.Settings.Volume;
        AppServices.Audio.Mute = AppServices.Settings.Mute;
        AppServices.Audio.Start();
        AppServices.Driver.SetActive(AppServices.Settings.Active);
        // статистика в control-канал (для страницы Dashboard и телефона)
        _statsCts = new CancellationTokenSource();
        var ct = _statsCts.Token;
        _ = Task.Run(async () =>
        {
            while (!ct.IsCancellationRequested)
            {
                try
                {
                    await Task.Delay(2000, ct);
                    var drv = AppServices.Driver.State;
                    Changed?.Invoke();
                }
                catch { break; }
            }
        }, ct);
        Changed?.Invoke();
    }

    private void OnStreamStopped()
    {
        _statsCts?.Cancel();
        AppServices.Audio.Stop();
        AppServices.Driver.SetActive(false);
        Changed?.Invoke();
    }

    private void OnClientDisconnected()
    {
        _statsCts?.Cancel();
        AppServices.Audio.Stop();
        AppServices.Media.StopUdp();
        AppServices.Media.StopTcp();
        AppServices.Driver.SetActive(false);
        PhoneDevice = "";
        Changed?.Invoke();
    }

    // ---------- локальные действия пользователя ----------

    public void SetVolume(double v)
    {
        v = Math.Clamp(v, 0, 1.5);
        AppServices.Settings.Volume = v;
        AppServices.Audio.Volume = v;
        SettingsService.Save(AppServices.Settings);
        Changed?.Invoke();
    }

    public void ToggleMute()
    {
        AppServices.Settings.Mute = !AppServices.Settings.Mute;
        AppServices.Audio.Mute = AppServices.Settings.Mute;
        SettingsService.Save(AppServices.Settings);
        Changed?.Invoke();
    }

    public void SetActive(bool active)
    {
        AppServices.Settings.Active = active;
        AppServices.Driver.SetActive(active);
        SettingsService.Save(AppServices.Settings);
        Changed?.Invoke();
    }

    public void SetTestSine(bool on) => AppServices.Audio.SetTestSine(on);

    // ---------- Bluetooth ----------

    /// <summary>Прикрепить уже установленную BT-сессию (страница «Подключение»).</summary>
    public void AttachBtSession(Bt.BtSession session)
    {
        lock (_lock)
        {
            _btSession?.Dispose();
            _btSession = session;
            TransportKind = "bt";
            PhoneDevice = session.DeviceName;
        }
        session.Changed += () => Changed?.Invoke();
        Changed?.Invoke();
    }

    /// <summary>Подключиться к телефону по Bluetooth (спаренное устройство).</summary>
    public async Task<bool> ConnectBluetoothAsync(string deviceId)
    {
        lock (_lock)
        {
            _btSession?.Dispose();
            _btSession = new Bt.BtSession();
        }
        var ok = await _btSession!.ConnectAsync(deviceId);
        if (ok)
        {
            TransportKind = "bt";
            PhoneDevice = _btSession.DeviceName;
            AppServices.Media.StopUdp();
            AppServices.Media.StopTcp();
            _btSession.StartAudio(AppServices.Settings);
            Changed?.Invoke();
        }
        return ok;
    }

    public void Dispose()
    {
        _statsCts?.Cancel();
        _btSession?.Dispose();
    }
}
