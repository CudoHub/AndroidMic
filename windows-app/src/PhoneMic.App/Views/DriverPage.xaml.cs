using System.Diagnostics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PhoneMic.App.Services;
using PhoneMic.Core.Logging;

namespace PhoneMic.App.Views;

public sealed partial class DriverPage : Page
{
    private System.Timers.Timer _poll = new(1000) { AutoReset = true };

    public DriverPage()
    {
        InitializeComponent();
        _poll.Elapsed += (_, _) => DispatcherQueue.TryEnqueue(Refresh);
        _poll.Start();
        Refresh();
    }

    private void Refresh()
    {
        var d = AppServices.Driver;
        if (d.IsPresent)
        {
            DriverInfoBar.Severity = InfoBarSeverity.Success;
            DriverInfoBar.Message = $"Подключён (версия интерфейса {d.Version}). Страница состояния: «Панель» и ниже.";
            var st = d.State;
            StateText.Text =
                $"path:    {d.DevicePath}\n" +
                $"version: {st.Version}\n" +
                $"active:  {st.Active}\n" +
                $"buffer:  {st.BufferedBytes} / {st.RingBytes} Б\n" +
                $"underruns: {st.Underruns}   overflows: {st.Overflows}\n" +
                $"period:  {st.PeriodMicrosec} мкс   rate: {st.SampleRate} Гц";
        }
        else
        {
            DriverInfoBar.Severity = InfoBarSeverity.Warning;
            DriverInfoBar.Message = "Драйвер не найден. Zoom/Discord/OBS увидят микрофон только после установки драйвера.";
            StateText.Text = "устройство не найдено (GUID 7C0A9E52-3B14-4D6F-9A8B-2E5C1D3F7A01)";
        }
        ViewModels.AppViewModel.Instance.UpdateDriverInfo();
    }

    private void Refresh_Click(object sender, RoutedEventArgs e)
    {
        AppServices.Driver.Reopen();
        Refresh();
    }

    private void Flush_Click(object sender, RoutedEventArgs e)
    {
        AppServices.Driver.Flush();
        AppLog.Info("DriverPage", "flush");
        Refresh();
    }

    private async void Install_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Установка драйвера",
            Content = "Будет запущен скрипт установки (pnputil + devcon) с правами администратора. " +
                      "Файлы драйвера должны лежать рядом с приложением (папка driver). Продолжить?",
            PrimaryButtonText = "Продолжить",
            CloseButtonText = "Отмена",
            DefaultButton = ContentDialogButton.Primary,
        };
        var result = await dlg.ShowAsync();
        if (result != ContentDialogResult.Primary) return;
        try
        {
            var psi = new ProcessStartInfo("powershell.exe",
                $"-NoProfile -ExecutionPolicy Bypass -File \"{FindInstallScript()}\"")
            {
                Verb = "runas",
                UseShellExecute = true,
            };
            Process.Start(psi);
        }
        catch (Exception ex)
        {
            AppLog.Error("DriverPage", "install launch failed", ex);
        }
    }

    private static string FindInstallScript()
    {
        var exeDir = AppContext.BaseDirectory;
        foreach (var candidate in new[]
                 {
                     Path.Combine(exeDir, "driver", "install_driver.ps1"),
                     Path.Combine(exeDir, "..", "..", "..", "..", "scripts", "install_driver.ps1"),
                     Path.Combine(exeDir, "install_driver.ps1"),
                 })
        {
            try { if (File.Exists(Path.GetFullPath(candidate))) return Path.GetFullPath(candidate); }
            catch { }
        }
        return "install_driver.ps1";
    }

    private async void Uninstall_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Удаление драйвера",
            Content = "Будет запущен скрипт удаления (devcon remove + pnputil) с правами администратора. Продолжить?",
            PrimaryButtonText = "Продолжить",
            CloseButtonText = "Отмена",
            DefaultButton = ContentDialogButton.Close,
        };
        var result = await dlg.ShowAsync();
        if (result != ContentDialogResult.Primary) return;
        try
        {
            var psi = new ProcessStartInfo("powershell.exe",
                $"-NoProfile -ExecutionPolicy Bypass -File \"{Path.Combine(AppContext.BaseDirectory, "driver", "uninstall_driver.ps1")}\"")
            {
                Verb = "runas",
                UseShellExecute = true,
            };
            Process.Start(psi);
        }
        catch (Exception ex)
        {
            AppLog.Error("DriverPage", "uninstall launch failed", ex);
        }
    }
}
