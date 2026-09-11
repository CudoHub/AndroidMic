using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace PhoneMic.App.DriverInterop;

/// <summary>P/Invoke-слой драйвера PhoneMic — docs/DRIVER_API.md.</summary>
public static class DriverApi
{
    public static readonly Guid DeviceInterfaceGuid =
        new("7C0A9E52-3B14-4D6F-9A8B-2E5C1D3F7A01");

    public const uint FileDeviceUnknown = 0x00000022;
    public const uint MethodBuffered = 0;
    public const uint FileAnyAccess = 0;

    public static uint CTL_CODE(uint func) =>
        (FileDeviceUnknown << 16) | (FileAnyAccess << 14) | (func << 2) | MethodBuffered;

    public static readonly uint IOCTL_PHONEMIC_GET_VERSION = CTL_CODE(0x800);
    public static readonly uint IOCTL_PHONEMIC_PUSH_SAMPLES = CTL_CODE(0x801);
    public static readonly uint IOCTL_PHONEMIC_GET_STATE = CTL_CODE(0x802);
    public static readonly uint IOCTL_PHONEMIC_SET_ACTIVE = CTL_CODE(0x803);
    public static readonly uint IOCTL_PHONEMIC_FLUSH = CTL_CODE(0x804);

    [StructLayout(LayoutKind.Sequential)]
    public struct PhonemicState
    {
        public uint Version;
        public uint Active;
        public uint Underruns;
        public uint Overflows;
        public uint BufferedBytes;
        public uint RingBytes;
        public uint PeriodMicrosec;
        public uint SampleRate;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern SafeFileHandle CreateFileW(
        string lpFileName, uint dwDesiredAccess, uint dwShareMode,
        IntPtr lpSecurityAttributes, uint dwCreationDisposition,
        uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool DeviceIoControl(
        SafeFileHandle hDevice, uint dwIoControlCode,
        IntPtr lpInBuffer, uint nInBufferSize,
        IntPtr lpOutBuffer, uint nOutBufferSize,
        out uint lpBytesReturned, IntPtr lpOverlapped);

    // --- SetupDi для поиска интерфейса устройства ---

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr SetupDiGetClassDevsW(
        ref Guid classGuid, IntPtr enumerator, IntPtr hwndParent, uint flags);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetupDiEnumDeviceInterfaces(
        IntPtr deviceInfoSet, IntPtr deviceInfoData,
        ref Guid interfaceClassGuid, int memberIndex,
        ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetupDiGetDeviceInterfaceDetailW(
        IntPtr deviceInfoSet, ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData,
        IntPtr deviceInterfaceDetailData, uint deviceInterfaceDetailDataSize,
        out uint requiredSize, IntPtr deviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetupDiDestroyDeviceInfoList(IntPtr deviceInfoSet);

    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVICE_INTERFACE_DATA
    {
        public int cbSize;
        public Guid InterfaceClassGuid;
        public uint Flags;
        public IntPtr Reserved;
    }

    public const uint DIGCF_PRESENT = 0x2;
    public const uint DIGCF_DEVICEINTERFACE = 0x10;
    public const uint GENERIC_READ = 0x80000000;
    public const uint GENERIC_WRITE = 0x40000000;
    public const uint FILE_SHARE_READ = 1;
    public const uint FILE_SHARE_WRITE = 2;
    public const uint OPEN_EXISTING = 3;

    /// <summary>Ищет путь интерфейса драйвера; null — не найден.</summary>
    public static string? FindDevicePath()
    {
        IntPtr set = IntPtr.Zero;
        try
        {
            var guid = DeviceInterfaceGuid;
            set = SetupDiGetClassDevsW(ref guid, IntPtr.Zero, IntPtr.Zero,
                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (set == new IntPtr(-1) || set == IntPtr.Zero) return null;

            for (int i = 0; ; i++)
            {
                var ifData = new SP_DEVICE_INTERFACE_DATA();
                ifData.cbSize = Marshal.SizeOf<SP_DEVICE_INTERFACE_DATA>();
                if (!SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref guid, i, ref ifData))
                    break; // no more

                // 1-й вызов — размер
                SetupDiGetDeviceInterfaceDetailW(set, ref ifData, IntPtr.Zero, 0, out uint size, IntPtr.Zero);
                IntPtr detailBuf = Marshal.AllocHGlobal((int)size);
                try
                {
                    // SP_DEVICE_INTERFACE_DETAIL_DATA_W: cbSize (UInt32) + char[1]
                    Marshal.WriteInt32(detailBuf, IntPtr.Size); // cbSize = sizeof(header)+sizeof(wchar) на 64-бит
                    if (SetupDiGetDeviceInterfaceDetailW(set, ref ifData, detailBuf, size, out _, IntPtr.Zero))
                    {
                        // строка лежит по смещению IntPtr.Size (после cbSize на 64-бит — 8)
                        IntPtr strPtr = IntPtr.Add(detailBuf, 4);
                        return Marshal.PtrToStringUni(strPtr);
                    }
                }
                finally
                {
                    Marshal.FreeHGlobal(detailBuf);
                }
            }
        }
        finally
        {
            if (set != IntPtr.Zero && set != new IntPtr(-1)) SetupDiDestroyDeviceInfoList(set);
        }
        return null;
    }
}
