namespace PhoneMic.Core.Audio;

/// <summary>RMS-метр: 0..1 со сглаживанием (для индикатора уровня).</summary>
public sealed class LevelMeter
{
    private float _smoothed;

    public float Next(ReadOnlySpan<short> samples)
    {
        if (samples.IsEmpty) return _smoothed;
        double acc = 0;
        for (int i = 0; i < samples.Length; i++)
            acc += (double)samples[i] * samples[i];
        double rms = Math.Sqrt(acc / samples.Length) / 32768.0;
        // нормировка: -20 dBFS ≈ 0.7
        float norm = (float)Math.Min(1.0, rms * 5.0);
        _smoothed = _smoothed * 0.7f + norm * 0.3f;
        return _smoothed;
    }

    public void Reset() => _smoothed = 0f;
}
