namespace PhoneMic.Core.Logging;

/// <summary>
/// Лог в файл %LOCALAPPDATA%\PhoneMic\logs\phonemic-YYYYMMDD.log + кольцевой буфер для UI.
/// ВАЖНО: токен никогда не пишется в лог.
/// </summary>
public static class AppLog
{
    private static readonly object Lock = new();
    private static readonly Queue<string> Ring = new();
    private static readonly string LogDir =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PhoneMic", "logs");

    public static event Action<string>? LineAdded;

    public static string LogDirectory => LogDir;

    public static void Info(string where, string msg) => Write("I", where, msg);
    public static void Warn(string where, string msg) => Write("W", where, msg);
    public static void Error(string where, string msg, Exception? ex = null) =>
        Write("E", where, msg + (ex != null ? $": {ex.Message}" : ""));

    private static void Write(string level, string where, string msg)
    {
        var line = $"{DateTime.Now:HH:mm:ss.fff} {level}/{where}: {msg}";
        lock (Lock)
        {
            if (Ring.Count >= 800) Ring.Dequeue();
            Ring.Enqueue(line);
            try
            {
                Directory.CreateDirectory(LogDir);
                File.AppendAllText(Path.Combine(LogDir, $"phonemic-{DateTime.Now:yyyyMMdd}.log"),
                    line + Environment.NewLine);
            }
            catch { /* лог не должен ломать приложение */ }
        }
        LineAdded?.Invoke(line);
    }

    public static IReadOnlyList<string> Dump()
    {
        lock (Lock) return Ring.ToList();
    }
}
