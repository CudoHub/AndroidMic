using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using PhoneMic.App.Services;
using PhoneMic.Core.Logging;

namespace PhoneMic.App;

/// <summary>Точка входа: инициализация сервисов, запуск слушателей.</summary>
public partial class App : Application
{
    public static Window? MainWindow { get; private set; }

    public App()
    {
        InitializeComponent();
        UnhandledException += (_, e) =>
        {
            AppLog.Error("App", "UNHANDLED: " + e.Message);
            e.Handled = true;
        };
    }

    protected override void OnLaunched(Microsoft.UI.Xaml.LaunchActivatedEventArgs args)
    {
        AppServices.Init();

        var s = SettingsService.Load();
        try
        {
            AppServices.ControlServer.Start(s.ControlPort);
        }
        catch (Exception ex)
        {
            AppLog.Error("App", $"control server start failed (port {s.ControlPort}): " + ex.Message);
        }
        if (s.DiscoveryEnabled)
        {
            try { AppServices.Discovery.Start(ProtocolDiscoveryPort); }
            catch (Exception ex) { AppLog.Error("App", "discovery start failed: " + ex.Message); }
        }

        MainWindow = new MainWindow();
        if (Environment.GetCommandLineArgs().Any(a => a.Equals("--minimized", StringComparison.OrdinalIgnoreCase)))
        {
            MainWindow.Activate();
            (MainWindow as MainWindow)?.HideToTray();
        }
        else
        {
            MainWindow.Activate();
        }
    }

    public const int ProtocolDiscoveryPort = Core.Protocol.ProtocolConstants.DiscoveryPort;
}
