using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PhoneMic.App.Services;
using PhoneMic.App.ViewModels;
using PhoneMic.Core.Logging;

namespace PhoneMic.App.Views;

public sealed partial class DashboardPage : Page
{
    public AppViewModel Vm { get; } = AppViewModel.Instance;

    public DashboardPage()
    {
        InitializeComponent();
    }

    private void Volume_ValueChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        if (e.NewValue != Vm.Volume) Vm.SetVolume(e.NewValue);
    }

    private void Mute_Click(object sender, RoutedEventArgs e) => Vm.ToggleMute();

    private void Active_Click(object sender, RoutedEventArgs e)
    {
        Vm.SetActive(!Vm.Active);
    }

    private void Stop_Click(object sender, RoutedEventArgs e)
    {
        AppLog.Info("Dashboard", "stop requested by user");
        // грубая остановка: гасим активность драйвера и тестовый сигнал
        Vm.SetActive(false);
        Vm.SetActive(true);
    }
}
