using System.Text.Json;
using System.Text.Json.Serialization;
using PhoneMic.Core.Logging;

namespace PhoneMic.App.Services;

/// <summary>Настройки приложения: %LOCALAPPDATA%\PhoneMic\settings.json.</summary>
public sealed class Settings
{
    public int ControlPort { get; set; } = 47821;
    public int MediaPort { get; set; } = 47822;
    public string Token { get; set; } = Core.Crypto.TokenUtil.Generate();
    public string Codec { get; set; } = "opus";             // opus | pcm
    public int FrameMs { get; set; } = 20;
    public int BitrateKbps { get; set; } = 48;
    public string JitterPreset { get; set; } = "balance";   // ultra | balance | stable
    public string MediaMode { get; set; } = "auto";         // auto | udp | tcp
    public string AdbPath { get; set; } = "";
    public string WfdPassphrase { get; set; } = NewPassphrase();
    public double Volume { get; set; } = 1.0;               // 0..1.5
    public bool Mute { get; set; }
    public bool Active { get; set; } = true;
    public bool Autostart { get; set; }
    public bool MinimizeToTray { get; set; } = true;
    public bool DiscoveryEnabled { get; set; } = true;
    public string PfxPassword { get; set; } = Guid.NewGuid().ToString("N");

    public static string NewPassphrase() =>
        "PHONEMIC-" + Convert.ToHexString(System.Security.Cryptography.RandomNumberGenerator.GetBytes(4));

    [JsonIgnore]
    public (int Min, int Target, int Max) Jitter =>
        JitterPreset switch
        {
            "ultra" => Core.Protocol.ProtocolConstants.JitterUltra,
            "stable" => Core.Protocol.ProtocolConstants.JitterStable,
            _ => Core.Protocol.ProtocolConstants.JitterBalance,
        };
}

public static class SettingsService
{
    private static readonly string Dir = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PhoneMic");
    private static readonly string Path_ = System.IO.Path.Combine(Dir, "settings.json");

    private static readonly object Lock = new();
    private static Settings? _current;

    public static Settings Load()
    {
        lock (Lock)
        {
            if (_current != null) return _current;
            try
            {
                if (File.Exists(Path_))
                {
                    var s = JsonSerializer.Deserialize<Settings>(File.ReadAllText(Path_));
                    if (s != null) { _current = s; return s; }
                }
            }
            catch (Exception ex)
            {
                Core.Logging.AppLog.Error("Settings", "load failed", ex);
            }
            _current = new Settings();
            Save(_current);
            return _current;
        }
    }

    public static void Save(Settings s)
    {
        lock (Lock)
        {
            try
            {
                Directory.CreateDirectory(Dir);
                var opts = new JsonSerializerOptions
                {
                    WriteIndented = true,
                    PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
                    DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
                };
                File.WriteAllText(Path_, JsonSerializer.Serialize(s, opts));
            }
            catch (Exception ex)
            {
                Core.Logging.AppLog.Error("Settings", "save failed", ex);
            }
        }
    }

    public static void Update(Action<Settings> f)
    {
        var s = Load();
        f(s);
        Save(s);
    }
}
