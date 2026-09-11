/*++
PhoneMic driver: control-устройство (IOCTL).
--*/
#pragma once
#include "common.h"

NTSTATUS PhonemicControlDeviceCreate(_In_ PDRIVER_OBJECT DriverObject);
VOID PhonemicControlDeviceCleanup(VOID);

DRIVER_DISPATCH PhonemicDispatchDeviceControl;
DRIVER_DISPATCH PhonemicDispatchCreate;
DRIVER_DISPATCH PhonemicDispatchClose;
