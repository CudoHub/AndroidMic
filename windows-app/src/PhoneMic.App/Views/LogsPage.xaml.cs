using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PhoneMic.App.Services;
using PhoneMic.App.ViewModels;

namespace PhoneMic.App.Views;

public sealed partial class LogsPage : Page
{
    public AppViewModel Vm { get; } = AppViewModel.Instance;

    public LogsPage()
    {
        InitializeComponent();
    }

    private void OpenLogs_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            System.Diagnostics.Process.Start("explorer.exe", Core.Logging.AppLog.LogDirectory);
        }
        catch { }
    }
}
