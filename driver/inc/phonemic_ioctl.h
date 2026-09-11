/*++
PhoneMic Virtual Microphone driver — IOCTL interface.
Зеркало docs/DRIVER_API.md. Используется WinUI-приложением (user mode) и INF.
--*/
#pragma once

#include <initguid.h>
#include <guiddef.h>

// {7C0A9E52-3B14-4D6F-9A8B-2E5C1D3F7A01}
DEFINE_GUID(GUID_DEVINTERFACE_PHONEMIC,
    0x7C0A9E52, 0x3B14, 0x4D6F, 0x9A, 0x8B, 0x2E, 0x5C, 0x1D, 0x3F, 0x7A, 0x01);

#define PHONEMIC_DEVICE_NAME        L"\\Device\\PhoneMic"
#define PHONEMIC_SYMLINK_NAME       L"\\DosDevices\\PhoneMic"
#define PHONEMIC_SYMLINK_USER_NAME  L"\\\\.\\PhoneMic"

#define FILE_DEVICE_PHONEMIC        0x00000022  // FILE_DEVICE_UNKNOWN
#define PHONEMIC_FUNCTION_BASE      0x800

#define PHONEMIC_CTL_CODE(func) \
    CTL_CODE(FILE_DEVICE_PHONEMIC, (PHONEMIC_FUNCTION_BASE + (func)), METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_PHONEMIC_GET_VERSION  PHONEMIC_CTL_CODE(0x00) // out: ULONG version (=1)
#define IOCTL_PHONEMIC_PUSH_SAMPLES PHONEMIC_CTL_CODE(0x01) // in: PCM bytes; out: ULONG accepted
#define IOCTL_PHONEMIC_GET_STATE    PHONEMIC_CTL_CODE(0x02) // out: PHONEMIC_STATE
#define IOCTL_PHONEMIC_SET_ACTIVE   PHONEMIC_CTL_CODE(0x03) // in: ULONG bool
#define IOCTL_PHONEMIC_FLUSH        PHONEMIC_CTL_CODE(0x04) // -

#define PHONEMIC_IOCTL_VERSION      1

#pragma pack(push, 1)
typedef struct _PHONEMIC_STATE {
    ULONG Version;
    ULONG Active;
    ULONG Underruns;
    ULONG Overflows;
    ULONG BufferedBytes;
    ULONG RingBytes;
    ULONG PeriodMicrosec;
    ULONG SampleRate;
} PHONEMIC_STATE, *PPHONEMIC_STATE;
#pragma pack(pop)

C_ASSERT(sizeof(PHONEMIC_STATE) == 32);
