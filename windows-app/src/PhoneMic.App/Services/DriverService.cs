using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using PhoneMic.App.DriverInterop;
using PhoneMic.Core.Logging;

namespace PhoneMic.App.Services;

/// <summary>
/// Взаимодействие с драйвером «PhoneMic Virtual Microphone» (docs/DRIVER_API.md):
/// поиск устройства, IOCTL push/state/active/flush.
/// </summary>
public sealed class DriverService : IDisposable
{
    private SafeFileHandle? _handle;
    private readonly object _lock = new();
    private readonly System.Timers.Timer _poll;

    public bool IsPresent { get; private set; }
    public string? DevicePath { get; private set; }
    public uint Version { get; private set; }
    public DriverApi.PhonemicState State { get; private set; }

    public event Action? StateChanged;

    public DriverService()
    {
        Reopen();
        _poll = new System.Timers.Timer(500) { AutoReset = true };
        _poll.Elapsed += (_, _) =>
        {
            try
            {
                Reopen();
                if (_handle != null && _handle.IsInvalid == false)
                {
                    if (TryGetState(out var st))
                    {
                        State = st;
                        StateChanged?.Invoke();
                    }
                }
            }
            catch { }
        };
        _poll.Start();
    }

    public void Reopen()
    {
        lock (_lock)
        {
            if (_handle != null && !_handle.IsInvalid && !_handle.IsClosed) { IsPresent = true; return; }
            IsPresent = false;
            DevicePath = DriverApi.FindDevicePath();
            if (DevicePath == null) return;
            var h = DriverApi.CreateFileW(DevicePath,
                DriverApi.GENERIC_READ | DriverApi.GENERIC_WRITE,
                DriverApi.FILE_SHARE_READ | DriverApi.FILE_SHARE_WRITE,
                IntPtr.Zero, DriverApi.OPEN_EXISTING, 0, IntPtr.Zero);
            if (h.IsInvalid)
            {
                AppLog.Warn("Driver", $"CreateFile failed for {DevicePath}: err={Marshal.GetLastWin32Error()}");
                return;
            }
            _handle = h;
            IsPresent = true;
            if (TryGetVersion(out var v)) Version = v;
            AppLog.Info("Driver", $"opened: {DevicePath} (v{Version})");
            StateChanged?.Invoke();
        }
    }

    private bool TryGetVersion(out uint version)
    {
        version = 0;
        var outBuf = Marshal.AllocHGlobal(4);
        try
        {
            if (!DeviceIoControlWrapped(DriverApi.IOCTL_PHONEMIC_GET_VERSION, IntPtr.Zero, 0, outBuf, 4, out _))
                return false;
            version = unchecked((uint)Marshal.ReadInt32(outBuf));
            return true;
        }
        finally { Marshal.FreeHGlobal(outBuf); }
    }

    public bool TryGetState(out DriverApi.PhonemicState st)
    {
        st = default;
        var outBuf = Marshal.AllocHGlobal(32);
        try
        {
            if (!DeviceIoControlWrapped(DriverApi.IOCTL_PHONEMIC_GET_STATE, IntPtr.Zero, 0, outBuf, 32, out _))
                return false;
            st = Marshal.PtrToStructure<DriverApi.PhonemicState>(outBuf);
            return true;
        }
        finally { Marshal.FreeHGlobal(outBuf); }
    }

    /// <summary>Подача PCM16LE mono 48k в кольцевой буфер драйвера.</summary>
    public bool PushSamples(byte[] pcm, int offset, int count)
    {
        lock (_lock)
        {
            if (_handle == null || _handle.IsInvalid) return false;
            var inBuf = Marshal.AllocHGlobal(count);
            try
            {
                Marshal.Copy(pcm, offset, inBuf, count);
                var outBuf = Marshal.AllocHGlobal(4);
                try
                {
                    return DeviceIoControlWrapped(DriverApi.IOCTL_PHONEMIC_PUSH_SAMPLES, inBuf, (uint)count, outBuf, 4, out _);
                }
                finally { Marshal.FreeHGlobal(outBuf); }
            }
            finally { Marshal.FreeHGlobal(inBuf); }
        }
    }

    public void SetActive(bool active)
    {
        lock (_lock)
        {
            if (_handle == null || _handle.IsInvalid) return;
            var inBuf = Marshal.AllocHGlobal(4);
            try
            {
                Marshal.WriteInt32(inBuf, active ? 1 : 0);
                DeviceIoControlWrapped(DriverApi.IOCTL_PHONEMIC_SET_ACTIVE, inBuf, 4, IntPtr.Zero, 0, out _);
            }
            finally { Marshal.FreeHGlobal(inBuf); }
        }
    }

    public void Flush()
    {
        lock (_lock)
        {
            if (_handle == null || _handle.IsInvalid) return;
            DeviceIoControlWrapped(DriverApi.IOCTL_PHONEMIC_FLUSH, IntPtr.Zero, 0, IntPtr.Zero, 0, out _);
        }
    }

    private bool DeviceIoControlWrapped(uint code, IntPtr inBuf, uint inSize, IntPtr outBuf, uint outSize, out uint returned)
    {
        return DriverApi.DeviceIoControl(_handle!, code, inBuf, inSize, outBuf, outSize, out returned, IntPtr.Zero);
    }

    public void Dispose()
    {
        _poll?.Dispose();
        try { _handle?.Dispose(); } catch { }
    }
}
