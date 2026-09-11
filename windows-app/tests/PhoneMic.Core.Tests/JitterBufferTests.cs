using PhoneMic.Core.Audio;
using Xunit;

namespace PhoneMic.Core.Tests;

public class JitterBufferTests
{
    [Fact]
    public void Reorder_Sequence()
    {
        var jb = new JitterBuffer((10, 10, 250));
        var f2 = new short[960]; f2[0] = 2;
        var f1 = new short[960]; f1[0] = 1;
        jb.Push(2, f2);
        jb.Push(1, f1); // пришёл позже — reorder
        // буфер не начнёт выдачу до target — target 10 мс, у нас 20 мс
        var a = jb.Drain();
        var b = jb.Drain();
        Assert.NotNull(a);
        Assert.Equal(1, a![0]);
        Assert.NotNull(b);
        Assert.Equal(2, b![0]);
    }

    [Fact]
    public void Loss_Counted()
    {
        var jb = new JitterBuffer((10, 10, 250));
        var f1 = new short[960];
        var f3 = new short[960];
        jb.Push(1, f1);
        jb.Push(3, f3); // seq 2 потерян
        jb.NotifyGap();
        Assert.Equal(1, jb.LostTotal);
    }

    [Fact]
    public void Underrun_Adapts_Target()
    {
        var jb = new JitterBuffer((10, 20, 250));
        int before = jb.TargetMs;
        jb.Drain(); // пусто → underrun → target растёт
        Assert.True(jb.TargetMs > before);
    }
}
