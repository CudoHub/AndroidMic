using Concentus.Structs;

namespace PhoneMic.Core.Audio;

/// <summary>
/// Opus-декодер (Concentus) 48 кГц mono + PLC. После старта потока отбрасывает
/// 312 сэмплов (preskip) — docs/PROTOCOL.md §6.2.
/// </summary>
public sealed class OpusDecoderWrapper : IDisposable
{
    private readonly OpusDecoder _decoder = new(ProtocolConstants.SampleRate, ProtocolConstants.Channels);
    private readonly short[] _pcm = new short[ProtocolConstants.SampleRate / 25]; // до 40 мс
    private bool _preskipDone;
    private int _preskipRemaining = ProtocolConstants.OpusPreskip;

    public void Reset() { _preskipDone = false; _preskipRemaining = ProtocolConstants.OpusPreskip; }

    /// <summary>Декодирует кадр. frameSamples — ожидаемый размер кадра в сэмплах (480/960).</summary>
    /// <returns>PCM16 samples; пустой массив — декодировать нечего.</returns>
    public short[] Decode(ReadOnlySpan<byte> opusFrame, int frameSamples)
    {
        int n = _decoder.Decode(opusFrame.IsEmpty ? null : opusFrame.ToArray(), 0,
            opusFrame.Length, _pcm, 0, frameSamples, false);
        ApplyPreskip(ref n);
        return _pcm.Take(n).ToArray();
    }

    /// <summary>Packet Loss Concealment: декодирование пустого кадра.</summary>
    public short[] DecodePlc(int frameSamples)
    {
        int n = _decoder.Decode(null, 0, 0, _pcm, 0, frameSamples, false);
        ApplyPreskip(ref n);
        return _pcm.Take(n).ToArray();
    }

    private void ApplyPreskip(ref int n)
    {
        if (_preskipDone) return;
        int skip = Math.Min(_preskipRemaining, n);
        _preskipRemaining -= skip;
        if (_preskipRemaining <= 0) _preskipDone = true;
        if (skip >= n) { n = 0; return; }
        Array.Copy(_pcm, skip, _pcm, 0, n - skip);
        n -= skip;
    }

    public void Dispose() { }
}
