using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using Microsoft.UI.Dispatching;
using PhoneMic.App.Services;
using PhoneMic.Core.Logging;

namespace PhoneMic.App.ViewModels;

/// <summary>Главная VM (синглтон): объединяет состояние всех страниц.</summary>
public partial class AppViewModel : ObservableObject
{
    public static AppViewModel Instance { get; } = new();

    private readonly DispatcherQueueTimer? _timer;

    [ObservableProperty] private string _sessionStatus = "Ожидание подключения…";
    [ObservableProperty] private string _phoneDevice = "—";
    [ObservableProperty] private string _transportKind = "—";
    [ObservableProperty] private string _codecInfo = "—";
    [ObservableProperty] private double _level;
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(RttLabel))]
    private int _rttMs;
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(LossLabel))]
    private double _lossPct;
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(BufferLabel))]
    private int _bufferedMs;
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(UnderrunsLabel))]
    private long _underruns;
    [ObservableProperty] private bool _streamActive;
    [ObservableProperty] private bool _driverPresent;
    [ObservableProperty] private string _driverInfo = "Не найден";
    [ObservableProperty] private string _driverStateInfo = "";
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(VolumeLabel))]
    private double _volume = 1.0;
    [ObservableProperty] private bool _mute;
    [ObservableProperty] private bool _active = true;
    [ObservableProperty] private bool _testSine;
    [ObservableProperty] private ObservableCollection<string> _logLines = new();
    [ObservableProperty] private string _tokenDisplay = "";
    [ObservableProperty] private string _certFingerprint = "";
    [ObservableProperty] private bool _wfdAdvertising;
    [ObservableProperty] private string _wfdStatus = "";

    private AppViewModel()
    {
        var queue = DispatcherQueue.GetForCurrentThread();
        _timer = queue.CreateTimer();
        _timer.Interval = TimeSpan.FromMilliseconds(300);
        _timer.Tick += (_, _) => Refresh();
        _timer.Start();

        AppLog.LineAdded += line =>
        {
            queue.TryEnqueue(() =>
            {
                LogLines.Insert(0, line);
                while (LogLines.Count > 400) LogLines.RemoveAt(LogLines.Count - 1);
            });
        };

        CertFingerprint = CertService.Fingerprint();
        TokenDisplay = "";
        _driverPresent = AppServices.Driver.IsPresent;
        UpdateDriverInfo();
        AppServices.Driver.StateChanged += () => queue.TryEnqueue(UpdateDriverInfo);
    }

    public void Refresh()
    {
        var s = AppServices.Session;
        var cs = AppServices.ControlServer;

        StreamActive = cs.StreamActive;
        PhoneDevice = cs.SessionActive ? (s.PhoneDevice == "" ? "подключён" : s.PhoneDevice) : "—";
        TransportKind = cs.SessionActive ? s.TransportKind : "—";
        CodecInfo = cs.SessionActive ? s.Codec : "—";
        SessionStatus = !cs.Running ? "Сервер остановлен"
            : StreamActive ? $"В эфире ({PhoneDevice}, {TransportKind})"
            : cs.SessionActive ? $"Телефон подключён ({PhoneDevice}) — нет потока"
            : "Ожидание подключения телефона…";

        Level = AppServices.Audio.Level * (AppServices.Audio.Mute ? 0 : 1);
        RttMs = cs.LastRttMs;
        LossPct = Math.Round(s.LossPct, 1);
        BufferedMs = s.BufferedMs;
        Underruns = AppServices.Media.Underruns;

        var st = AppServices.Driver.State;
        DriverStateInfo = AppServices.Driver.IsPresent
            ? $"buffered {st.BufferedBytes}/{st.RingBytes} Б, underruns {st.Underruns}, overflows {st.Overflows}, период {st.PeriodMicrosec} мкс"
            : "";
    }

    public void UpdateDriverInfo()
    {
        DriverPresent = AppServices.Driver.IsPresent;
        DriverInfo = AppServices.Driver.IsPresent
            ? $"Установлен (v{AppServices.Driver.Version}) — «PhoneMic Virtual Microphone»"
            : "Драйвер не найден — установите его (см. страницу «Драйвер»)";
    }

    // ---- команды ----

    public void SetVolume(double v) => AppServices.Session.SetVolume(v);
    public void ToggleMute()
    {
        AppServices.Session.ToggleMute();
        Mute = AppServices.Settings.Mute;
    }
    public void SetActive(bool active)
    {
        AppServices.Session.SetActive(active);
        Active = active;
    }
    public void ToggleTestSine()
    {
        TestSine = !TestSine;
        AppServices.Session.SetTestSine(TestSine);
    }
    public void SetTestSine(bool on)
    {
        TestSine = on;
        AppServices.Session.SetTestSine(on);
    }

    public void RevealToken() => TokenDisplay = AppServices.Settings.Token;
    public void HideToken() => TokenDisplay = "";
    public string RegenerateToken()
    {
        AppServices.Settings.Token = Core.Crypto.TokenUtil.Generate();
        SettingsService.Save(AppServices.Settings);
        TokenDisplay = AppServices.Settings.Token;
        AppLog.Warn("Security", "token regenerated — обновите токен на телефоне!");
        return TokenDisplay;
    }

    public void SavePorts(int controlPort, int mediaPort)
    {
        SettingsService.Update(s => { s.ControlPort = controlPort; s.MediaPort = mediaPort; });
        try { AppServices.ControlServer.Start(controlPort); }
        catch (Exception ex) { AppLog.Error("Ports", "restart server: " + ex.Message); }
    }

    public void SaveAudioSettings(string codec, int frameMs, int bitrateKbps, string jitter)
    {
        SettingsService.Update(s => { s.Codec = codec; s.FrameMs = frameMs; s.BitrateKbps = bitrateKbps; s.JitterPreset = jitter; });
    }

    public void ToggleWfd()
    {
        if (AppServices.WifiDirect.Advertising) AppServices.WifiDirect.Stop();
        else AppServices.WifiDirect.Start();
        WfdAdvertising = AppServices.WifiDirect.Advertising;
        WfdStatus = AppServices.WifiDirect.Status;
    }

    public string WfdPassphrase => AppServices.Settings.WfdPassphrase;

    // ---- вычисляемые строки для UI ----
    // WinUI 3: x:Bind НЕ поддерживает StringFormat (это WPF-only), поэтому
    // форматирование делается здесь; [NotifyPropertyChangedFor] выше гарантирует,
    // что метки обновляются вместе с исходными значениями.
    public string RttLabel => $"RTT: {RttMs} мс";
    public string LossLabel => $"Потери: {LossPct} %";
    public string BufferLabel => $"Буфер: {BufferedMs} мс";
    public string UnderrunsLabel => $"Недоборы: {Underruns}";
    public string VolumeLabel => Volume.ToString("P0");

    public void DisposeUi() { /* таймер живёт до выхода приложения */ }
}
