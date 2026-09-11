using System.Diagnostics;
using PhoneMic.Core.Logging;

namespace PhoneMic.App.Services;

/// <summary>
/// USB через ADB: настройка adb reverse, чтобы телефон подключался к 127.0.0.1.
/// Требуется USB-отладка на телефоне и adb.exe (путь в настройках или PATH).
/// </summary>
public sealed class UsbAdbService
{
    public string? FindAdb()
    {
        var s = SettingsService.Load();
        if (!string.IsNullOrEmpty(s.AdbPath) && File.Exists(s.AdbPath)) return s.AdbPath;

        var candidates = new[]
        {
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "Android", "Sdk", "platform-tools", "adb.exe"),
            Path.Combine("C:", "adb", "adb.exe"),
        };
        foreach (var c in candidates)
            if (File.Exists(c)) return c;

        // PATH
        var pathEnv = Environment.GetEnvironmentVariable("PATH") ?? "";
        foreach (var dir in pathEnv.Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            try
            {
                var p = Path.Combine(dir.Trim(), "adb.exe");
                if (File.Exists(p)) return p;
            }
            catch { }
        }
        return null;
    }

    public bool DeviceConnected()
    {
        var adb = FindAdb();
        if (adb == null) return false;
        var (ok, output) = Run(adb, "devices");
        return ok && output.Contains("device", StringComparison.Ordinal);
    }

    /// <summary>Настраивает adb reverse для control- и media-портов.</summary>
    public (bool Ok, string Message) ConfigureReverse()
    {
        var adb = FindAdb();
        if (adb == null)
            return (false, "adb.exe не найден. Укажите путь в настройках (платформ-тулзы Android SDK).");
        Run(adb, "start-server");
        var s = SettingsService.Load();
        var r1 = Run(adb, $"reverse tcp:{s.ControlPort} tcp:{s.ControlPort}");
        var r2 = Run(adb, $"reverse tcp:{s.MediaPort} tcp:{s.MediaPort}");
        if (r1.ok && r2.ok)
        {
            AppLog.Info("Adb", "reverse configured");
            return (true, "adb reverse настроен: телефон может подключаться к 127.0.0.1");
        }
        var msg = (r1.output + " " + r2.output).Trim();
        AppLog.Warn("Adb", "reverse failed: " + msg);
        return (false, "Ошибка adb: " + msg);
    }

    private static (bool ok, string output) Run(string adb, string args)
    {
        try
        {
            var psi = new ProcessStartInfo(adb, args)
            {
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
            };
            using var p = Process.Start(psi)!;
            var output = p.StandardOutput.ReadToEnd() + p.StandardError.ReadToEnd();
            p.WaitForExit(5000);
            return (p.ExitCode == 0, output);
        }
        catch (Exception ex)
        {
            return (false, ex.Message);
        }
    }
}
