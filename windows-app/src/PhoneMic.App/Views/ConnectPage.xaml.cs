using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PhoneMic.App.Services;
using PhoneMic.App.Services.Bt;
using PhoneMic.Core.Logging;

namespace PhoneMic.App.Views;

public sealed partial class ConnectPage : Page
{
    private System.Timers.Timer _poll = new(1000) { AutoReset = true };
    private System.Collections.Generic.List<Windows.Devices.Enumeration.DeviceInformation> _btDevices = new();

    public ConnectPage()
    {
        InitializeComponent();
        _poll.Elapsed += (_, _) => DispatcherQueue.TryEnqueue(RefreshStatus);
        _poll.Start();
        RefreshStatus();
        DiscoveryToggle.IsOn = AppServices.Discovery.Running;
        _ = LoadBtAsync();
    }

    private void RefreshStatus()
    {
        var cs = AppServices.ControlServer;
        ServerStatus.Text = cs.Running
            ? $"Слушает порт {AppServices.Settings.ControlPort} (TLS)"
            : "Остановлен";
        ClientInfo.Text = "Клиент: " + (cs.SessionActive ? cs.ClientInfo : "—");
    }

    private void Discovery_Toggled(object sender, RoutedEventArgs e)
    {
        if (DiscoveryToggle.IsOn) AppServices.Discovery.Start(Core.Protocol.ProtocolConstants.DiscoveryPort);
        else AppServices.Discovery.Stop();
        SettingsService.Update(s => s.DiscoveryEnabled = DiscoveryToggle.IsOn);
    }

    private async void RefreshBt_Click(object sender, RoutedEventArgs e) => await LoadBtAsync();

    private async System.Threading.Tasks.Task LoadBtAsync()
    {
        try
        {
            _btDevices = (await BtSession.FindPhonesAsync()).ToList();
            BtList.Items.Clear();
            foreach (var d in _btDevices)
                BtList.Items.Add(new ListViewItem { Content = $"{d.Name} ({d.Id[..Math.Min(24, d.Id.Length)]}…)" });
            BtStatus.Text = _btDevices.Count == 0
                ? "Телефоны с PhoneMic-службой не найдены. Сопрягите телефон с ПК и включите Bluetooth на телефоне."
                : $"Найдено: {_btDevices.Count}";
        }
        catch (Exception ex)
        {
            BtStatus.Text = "Ошибка Bluetooth: " + ex.Message;
            AppLog.Error("Connect", "bt list", ex);
        }
    }

    private async void ConnectBt_Click(object sender, RoutedEventArgs e)
    {
        if (BtList.SelectedIndex < 0 || BtList.SelectedIndex >= _btDevices.Count) return;
        var dev = _btDevices[BtList.SelectedIndex];
        BtStatus.Text = $"Подключение к {dev.Name}…";
        var s = AppServices.Settings;
        var session = new BtSession();
        var ok = await session.ConnectAsync(dev.Id);
        if (!ok)
        {
            BtStatus.Text = "Не удалось подключиться (см. журнал)";
            session.Dispose();
            return;
        }
        ok = await session.StartSession(s);
        if (!ok)
        {
            BtStatus.Text = "Handshake не прошёл — проверьте токен";
            session.Dispose();
            return;
        }
        AppServices.Session.AttachBtSession(session);
        session.StartAudio(s);
        BtStatus.Text = $"Подключено: {session.DeviceName}";
    }
}
