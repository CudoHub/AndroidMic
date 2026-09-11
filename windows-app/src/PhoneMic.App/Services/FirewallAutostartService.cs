using System.Diagnostics;
using PhoneMic.Core.Logging;

namespace PhoneMic.App.Services;

/// <summary>Правила брандмауэра для входящих TCP/UDP портов (netsh, требует прав администратора).</summary>
public static class FirewallService
{
    public static (bool Ok, string Message) AddRules()
    {
        var s = SettingsService.Load();
        var exe = "netsh";
        var results = new List<string>();
        var rules = new[]
        {
            ("PhoneMic Control TCP", $"direction=in action=allow protocol=TCP localport={s.ControlPort}"),
            ("PhoneMic Media TCP",  $"direction=in action=allow protocol=TCP localport={s.MediaPort}"),
            ("PhoneMic Media UDP",  $"direction=in action=allow protocol=UDP localport={s.MediaPort}"),
        };
        foreach (var (name, args) in rules)
        {
            var psi = new ProcessStartInfo(exe, $"advfirewall firewall add rule name=\"{name}\" {args}")
            {
                Verb = "runas",
                UseShellExecute = true,
                CreateNoWindow = true,
            };
            try
            {
                using var p = Process.Start(psi)!;
                p.WaitForExit(10000);
                results.Add($"{name}: exit {p.ExitCode}");
            }
            catch (Exception ex)
            {
                return (false, "Не удалось выполнить netsh (нужно согласие UAC): " + ex.Message);
            }
        }
        AppLog.Info("Firewall", string.Join("; ", results));
        return (true, "Правила добавлены: " + string.Join("; ", results));
    }
}

/// <summary>Автозапуск: HKCU Run.</summary>
public static class AutostartService
{
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "PhoneMic";

    public static void Set(bool enabled)
    {
        try
        {
            using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(RunKey, true);
            if (key == null) return;
            if (enabled)
            {
                var exe = Environment.ProcessPath;
                if (!string.IsNullOrEmpty(exe))
                    key.SetValue(ValueName, $"\"{exe}\" --minimized");
            }
            else key.DeleteValue(ValueName, false);
        }
        catch (Exception ex)
        {
            Core.Logging.AppLog.Warn("Autostart", ex.Message);
        }
    }
}
