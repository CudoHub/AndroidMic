using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PhoneMic.App.Services;
using PhoneMic.Core.Logging;

namespace PhoneMic.App.Views;

public sealed partial class TransportPage : Page
{
    public TransportPage()
    {
        InitializeComponent();
        var s = AppServices.Settings;
        CtlPortBox.Text = s.ControlPort.ToString();
        MediaPortBox.Text = s.MediaPort.ToString();
        MediaModeBox.SelectedIndex = s.MediaMode switch { "udp" => 1, "tcp" => 2, _ => 0 };
        AdbPathBox.Text = s.AdbPath;
    }

    private void Port_TextChanged(object sender, TextChangedEventArgs e)
    {
        // валидация на лету: цифры
        if (sender is TextBox tb && !int.TryParse(tb.Text, out _))
        {
            tb.Text = new string(tb.Text.Where(char.IsDigit).ToArray());
        }
    }

    private void ApplyPorts_Click(object sender, RoutedEventArgs e)
    {
        if (!int.TryParse(CtlPortBox.Text, out var ctl) || !int.TryParse(MediaPortBox.Text, out var media) ||
            ctl is < 1 or > 65535 || media is < 1 or > 65535)
        {
            FwStatus.Text = "Порты должны быть числами 1..65535";
            return;
        }
        VmSave(ctl, media);
        FwStatus.Text = $"Порты сохранены: control={ctl}, media={media}";
        AppLog.Info("Transport", $"ports: control={ctl} media={media}");
    }

    private void VmSave(int ctl, int media) => App.ViewModels.AppViewModel.Instance.SavePorts(ctl, media);

    private void MediaMode_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        var mode = MediaModeBox.SelectedIndex switch { 1 => "udp", 2 => "tcp", _ => "auto" };
        SettingsService.Update(s => s.MediaMode = mode);
    }

    private void Wfd_Click(object sender, RoutedEventArgs e)
    {
        App.ViewModels.AppViewModel.Instance.ToggleWfd();
        WfdStatusText.Text = AppServices.WifiDirect.Status;
        WfdButton.Content = AppServices.WifiDirect.Advertising ? "Выключить публикацию" : "Включить публикацию";
    }

    private void WfdPass_Click(object sender, RoutedEventArgs e)
    {
        var pass = AppServices.Settings.WfdPassphrase;
        WfdStatusText.Text = $"Пароль группы WPA2: {pass} (SSID: DIRECT-…{Environment.MachineName})";
    }

    private void AdbReverse_Click(object sender, RoutedEventArgs e)
    {
        var (ok, msg) = AppServices.Adb.ConfigureReverse();
        AdbStatus.Text = msg;
    }

    private void AdbCheck_Click(object sender, RoutedEventArgs e)
    {
        AdbStatus.Text = AppServices.Adb.DeviceConnected()
            ? "Устройство Android видно в adb"
            : "adb не видит устройство (проверьте USB-отладку и кабель)";
    }

    private void AdbSave_Click(object sender, RoutedEventArgs e)
    {
        var path = AdbPathBox.Text.Trim();
        SettingsService.Update(s => s.AdbPath = path);
        AdbStatus.Text = File.Exists(path) ? "Путь сохранён" : "Файл не существует — путь всё равно сохранён";
    }

    private void Firewall_Click(object sender, RoutedEventArgs e)
    {
        var (ok, msg) = FirewallService.AddRules();
        FwStatus.Text = msg;
    }
}
