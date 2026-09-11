using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PhoneMic.App.Services;
using PhoneMic.Core.Crypto;

namespace PhoneMic.App.Views;

public sealed partial class SecurityPage : Page
{
    public SecurityPage()
    {
        InitializeComponent();
        var fp = Services.CertService.Fingerprint();
        FpBox.Text = string.Join(" ", Enumerable.Range(0, Math.Min(32, fp.Length / 2))
            .Select(i => fp[(i * 2)..(i * 2 + 2)]));
    }

    private void Show_Click(object sender, RoutedEventArgs e)
    {
        TokenBox.Text = AppServices.Settings.Token;
    }

    private void Hide_Click(object sender, RoutedEventArgs e)
    {
        TokenBox.Text = "••••••••••••••••••••••••";
    }

    private void Regen_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Сгенерировать токен заново?",
            Content = "Все ранее подключённые телефоны перестанут авторизовываться — нужно будет ввести новый токен.",
            PrimaryButtonText = "Сгенерировать",
            CloseButtonText = "Отмена",
            DefaultButton = ContentDialogButton.Close,
        };
        _ = dlg.ShowAsync();
        dlg.PrimaryButtonClick += (_, _) =>
        {
            TokenBox.Text = ViewModels.AppViewModel.Instance.RegenerateToken();
        };
    }

    private void CopyFp_Click(object sender, RoutedEventArgs e)
    {
        var fp = Services.CertService.Fingerprint();
        var package = new Windows.ApplicationModel.DataTransfer.DataPackage();
        package.SetText(fp);
        Windows.ApplicationModel.DataTransfer.Clipboard.SetContent(package);
    }
}
