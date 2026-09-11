namespace PhoneMic.Core.Audio;

/// <summary>
/// Адаптивный джиттер-буфер кадров по 10 мс (PCM16). Приём через Push (по seq),
/// выдача через Drain раз в 5 мс. Потери → колбэк PLC. Адаптация target: +10 мс
/// при недоборе, −5 мс за каждые 5 с без потерь (docs/PROTOCOL.md §7).
/// </summary>
public sealed class JitterBuffer
{
    private readonly object _lock = new();
    private readonly Queue<short[]> _queue = new();
    private readonly SortedDictionary<uint, short[]> _pending = new();
    private long _bufferedSamples;

    private readonly int _minMs;
    private readonly int _maxMs;
    private float _targetMs;
    private readonly int _frameMs = 10;

    private uint _lastSeq;      // последний выданный seq
    private bool _started;
    private long _lostTotal;
    private long _receivedTotal;
    private long _lastLossTick;
    private long _underruns;

    public JitterBuffer((int Min, int Target, int Max) preset)
    {
        _minMs = preset.Min;
        _maxMs = preset.Max;
        _targetMs = preset.Target;
    }

    public int TargetMs { get { lock (_lock) return (int)_targetMs; } }
    public long LostTotal { get { lock (_lock) return _lostTotal; } }
    public long ReceivedTotal { get { lock (_lock) return _receivedTotal; } }
    public long Underruns => Interlocked.Read(ref _underruns);
    public int BufferedMs { get { lock (_lock) return (int)(_bufferedSamples / 48); } }

    /// <summary>Push кадра PCM16 (произвольная длительность, обычно 960 сэмплов). Reorder внутри окна 512.</summary>
    public void Push(uint seq, short[] frame)
    {
        lock (_lock)
        {
            _receivedTotal++;
            if (_started && seq <= _lastSeq) return; // дубликат/поздний
            if (!_pending.TryAdd(seq, frame)) return;
            // если накопили в порядке — переносим в очередь выдачи
            while (_pending.TryGetValue(_lastSeq + 1, out var next))
            {
                _pending.Remove(_lastSeq + 1);
                _lastSeq++;
                _queue.Enqueue(next);
                _bufferedSamples += next.Length;
            }
        }
    }

    /// <summary>Забирает кадры, если буфер достиг target. Возвращает null — недобор.</summary>
    public short[]? Drain()
    {
        lock (_lock)
        {
            if (!_started)
            {
                if (_bufferedSamples / 48 >= _targetMs) _started = true;
                else return null;
            }
            if (_queue.Count == 0)
            {
                Interlocked.Increment(ref _underruns);
                BumpTarget();
                return null;
            }
            var frame = _queue.Dequeue();
            _bufferedSamples -= frame.Length;
            ShrinkIfStable();
            return frame;
        }
    }

    /// <summary>Готов ли буфер к выдаче (для цикла).</summary>
    public bool HasOutput
    {
        get { lock (_lock) return _started && _queue.Count > 0; }
    }

    /// <summary>Колбэк PLC: уведомить о разрыве seq (вызывает владелец с кадром тишины).</summary>
    public void NotifyGap()
    {
        lock (_lock)
        {
            _lostTotal++;
            BumpTarget();
        }
    }

    private void BumpTarget()
    {
        _targetMs = Math.Min(_targetMs + 10, _maxMs);
        _lastLossTick = Environment.TickCount64;
    }

    private void ShrinkIfStable()
    {
        if (Environment.TickCount64 - _lastLossTick > 5000 && _targetMs > _minMs)
        {
            _targetMs = Math.Max(_targetMs - 5, _minMs);
            _lastLossTick = Environment.TickCount64;
        }
    }
}
