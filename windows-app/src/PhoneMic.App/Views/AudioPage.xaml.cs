using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PhoneMic.App.Services;

namespace PhoneMic.App.Views;

public sealed partial class AudioPage : Page
{
    public AudioPage()
    {
        InitializeComponent();
        var s = AppServices.Settings;
        CodecBox.SelectedIndex = s.Codec == "pcm" ? 1 : 0;
        FrameBox.SelectedIndex = s.FrameMs == 10 ? 0 : 1;
        BitrateBox.SelectedIndex = s.BitrateKbps switch
        {
            24 => 0, 32 => 1, 48 => 2, 64 => 3, 96 => 4, 128 => 5, _ => 2
        };
        JitterBox.SelectedIndex = s.JitterPreset switch { "ultra" => 0, "stable" => 2, _ => 1 };
        TestSineToggle.IsOn = false;
    }

    private void Changed(object sender, SelectionChangedEventArgs e)
    {
        var codec = CodecBox.SelectedIndex == 1 ? "pcm" : "opus";
        var frameMs = FrameBox.SelectedIndex == 0 ? 10 : 20;
        var bitrate = BitrateBox.SelectedIndex switch { 0 => 24, 1 => 32, 3 => 64, 4 => 96, 5 => 128, _ => 48 };
        var jitter = JitterBox.SelectedIndex switch { 0 => "ultra", 2 => "stable", _ => "balance" };
        ViewModels.AppViewModel.Instance.SaveAudioSettings(codec, frameMs, bitrate, jitter);
    }

    private void TestSine_Toggled(object sender, RoutedEventArgs e)
    {
        ViewModels.AppViewModel.Instance.SetTestSine(TestSineToggle.IsOn);
    }
}
