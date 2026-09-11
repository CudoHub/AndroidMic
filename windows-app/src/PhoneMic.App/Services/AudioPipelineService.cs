using PhoneMic.Core.Audio;
using PhoneMic.Core.Logging;
using PhoneMic.Core.Protocol;

namespace PhoneMic.App.Services;

/// <summary>
/// Аудиотракт: JitterBuffer → декодер (Opus/PCM) → preskip → gain/mute →
/// LevelMeter → драйвер (или тестовый сигнал). Тик 5 мс.
/// </summary>
public sealed class AudioPipelineService : IDisposable
{
    private readonly object _lock = new();
    private JitterBuffer? _jitter;
    private OpusDecoderWrapper? _opus;
    private readonly LevelMeter _meter = new();
    private readonly System.Timers.Timer _timer;

    private volatile bool _streaming;
    private volatile bool _testSine;
    private double _sinePhase;

    public double Volume { get; set; } = 1.0;   // 0..1.5, применяется к PCM перед драйвером
    public bool Mute { get; set; }              // локальный мьют ПК
    public Func<int, short[]?>? PcmSource { get; set; } // источник кадров 10 мс (из медиа-сервисов)

    public float Level { get; private set; }
    public bool DriverOk { get; set; }

    public event Action? Ticked;

    public AudioPipelineService()
    {
        _timer = new System.Timers.Timer(5) { AutoReset = true };
        _timer.Elapsed += (_, _) => Tick();
    }

    public void Configure((int Min, int Target, int Max) jitterPreset, string codec)
    {
        lock (_lock)
        {
            _jitter = new JitterBuffer(jitterPreset);
            _opus = codec == "opus" ? new OpusDecoderWrapper() : null;
        }
    }

    public void Start()
    {
        lock (_lock)
        {
            if (_jitter == null) throw new InvalidOperationException("Configure() first");
            _streaming = true;
        }
        _timer.Start();
        AppLog.Info("Audio", "pipeline started");
    }

    public void Stop()
    {
        _streaming = false;
        _timer.Stop();
        lock (_lock)
        {
            _jitter = null;
            _opus?.Dispose();
            _opus = null;
        }
        Level = 0;
        AppLog.Info("Audio", "pipeline stopped");
    }

    public JitterBuffer? Jitter { get { lock (_lock) return _jitter; } }

    public void SetTestSine(bool on) => _testSine = on;

    private void Tick()
    {
        try
        {
            // кадр 10 мс = 960 сэмплов
            short[] frame;
            if (_testSine)
            {
                frame = new short[ProtocolConstants.PcmChunkSamples];
                lock (_lock)
                {
                    for (int i = 0; i < frame.Length; i++)
                    {
                        _sinePhase += 2 * Math.PI * 440.0 / ProtocolConstants.SampleRate;
                        if (_sinePhase > 2 * Math.PI) _sinePhase -= 2 * Math.PI;
                        // -20 dBFS ≈ 0.1 амплитуды
                        frame[i] = (short)(Math.Sin(_sinePhase) * 3276);
                    }
                }
            }
            else
            {
                var src = PcmSource;
                if (src == null) return;
                var got = src(0);
                if (got == null) return;
                frame = got;
            }
            // PLC: если буфер разорван — владельцем джиттера это уже обработано

            // gain/mute + метр
            double vol = Mute ? 0 : Volume;
            lock (_lock)
            {
                if (vol != 1.0)
                {
                    for (int i = 0; i < frame.Length; i++)
                    {
                        int v = (int)(frame[i] * vol);
                        frame[i] = (short)Math.Clamp(v, short.MinValue, short.MaxValue);
                    }
                }
            }
            Level = _meter.Next(frame);

            // подача в драйвер
            var drv = AppServices.Driver;
            if (drv != null && drv.IsPresent)
            {
                var bytes = new byte[frame.Length * 2];
                Buffer.BlockCopy(frame, 0, bytes, 0, bytes.Length);
                DriverOk = drv.PushSamples(bytes, 0, bytes.Length);
            }
            else DriverOk = false;

            Ticked?.Invoke();
        }
        catch (Exception ex)
        {
            AppLog.Error("Audio", "tick", ex);
        }
    }

    public void Dispose() => _timer.Dispose();
}
