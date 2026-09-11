using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PhoneMic.App.Services;
using PhoneMic.App.ViewModels;
using PhoneMic.Core.Logging;

namespace PhoneMic.App;

/// <summary>Главное окно: NavigationView + страницы. Закрытие → в трей (по настройке).</summary>
public sealed partial class MainWindow : Window
{
    public AppViewModel Vm { get; } = AppViewModel.Instance;

    private Hardcodet.Wpf.TaskbarNotification.TaskbarIcon? _tray;
    private bool _realExit;

    public MainWindow()
    {
        InitializeComponent();
        Title = "PhoneMic — телефон как микрофон";
        ContentFrame.Navigate(typeof(Views.DashboardPage));

        Activated += (_, _) => { };
        Closed += (_, _) =>
        {
            if (!_realExit)
            {
                // обычное закрытие окна = свернуть в трей (если включено)
            }
            Vm.DisposeUi();
        };
    }

    public void HideToTray()
    {
        try
        {
            if (_tray == null) CreateTray();
            AppWindow.Hide();
        }
        catch (Exception ex)
        {
            AppLog.Warn("Tray", ex.Message);
        }
    }

    private void CreateTray()
    {
        _tray = new Hardcodet.Wpf.TaskbarNotification.TaskbarIcon
        {
            ToolTipText = "PhoneMic",
            PopupActivation = Hardcodet.Wpf.TaskbarNotification.PopupActivationMode.RightClick,
        };
        _tray.LeftClickCommand = new RelayCommand(ShowFromTray);
    }

    public void ShowFromTray()
    {
        AppWindow.Show();
        Activate();
    }

    public void ExitApp()
    {
        _realExit = true;
        _tray?.Dispose();
        Close();
        Application.Current.Exit();
    }

    private void Nav_SelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        var tag = (sender.SelectedItem as NavigationViewItem)?.Tag?.ToString();
        Type? page = tag switch
        {
            "dashboard" => typeof(Views.DashboardPage),
            "connect" => typeof(Views.ConnectPage),
            "transport" => typeof(Views.TransportPage),
            "audio" => typeof(Views.AudioPage),
            "security" => typeof(Views.SecurityPage),
            "driver" => typeof(Views.DriverPage),
            "logs" => typeof(Views.LogsPage),
            _ => null,
        };
        if (page != null) ContentFrame.Navigate(page);
    }
}

/// <summary>Мини-команда для трея.</summary>
public sealed class RelayCommand : System.Windows.Input.ICommand
{
    private readonly Action _exec;
    public RelayCommand(Action exec) => _exec = exec;
    public event EventHandler? CanExecuteChanged;
    public bool CanExecute(object? parameter) => true;
    public void Execute(object? parameter) => _exec();
}
