/*++
PhoneMic driver: control-устройство (\Device\PhoneMic + интерфейс GUID),
кольцевой буфер 100 мс, IOCTL-обработчики. Данные PCM подаёт WinUI-приложение.
--*/
#include "common.h"
#include "control.h"

// ----------------------------- кольцевой буфер ------------------------------

static KSPIN_LOCK g_RingLock;
static PUCHAR g_Ring = NULL;
static ULONG g_RingWritePos = 0;   // байт
static ULONG g_RingFill = 0;       // байт
static ULONG g_Underruns = 0;
static ULONG g_Overflows = 0;
static ULONG g_Active = 1;         // приложение подаёт данные

LONG g_CaptureStreamActive = 0;    // 1, если открыт KS capture-поток и RUN

#pragma code_seg("PAGE")
NTSTATUS PhonemicRingInit(VOID)
{
    PAGED_CODE();
    ASSERT(g_Ring == NULL);
    g_Ring = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, PHONEMIC_RING_BYTES, PHONEMIC_TAG_RING);
    if (g_Ring == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(g_Ring, PHONEMIC_RING_BYTES);
    KeInitializeSpinLock(&g_RingLock);
    g_RingWritePos = 0;
    g_RingFill = 0;
    g_Underruns = 0;
    g_Overflows = 0;
    g_Active = 1;
    return STATUS_SUCCESS;
}
#pragma code_seg()

VOID PhonemicRingReset(VOID)
{
    if (g_Ring == NULL) return;
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_RingLock, &oldIrql);
    g_RingWritePos = 0;
    g_RingFill = 0;
    g_Underruns = 0;
    g_Overflows = 0;
    KeReleaseSpinLock(&g_RingLock, oldIrql);
}

/* Копирует из кольца до maxBytes (вызывается в DPC). Возвращает скопированные байты. */
_IRQL_requires_max_(DISPATCH_LEVEL)
ULONG PhonemicRingPull(_Out_writes_bytes_(maxBytes) PUCHAR dest, _In_ ULONG maxBytes)
{
    KIRQL oldIrql;
    ULONG copied = 0;
    KeAcquireSpinLock(&g_RingLock, &oldIrql);
    if (g_RingFill > 0)
    {
        copied = min(maxBytes, g_RingFill);
        ULONG pos = (g_RingWritePos - g_RingFill + PHONEMIC_RING_BYTES) % PHONEMIC_RING_BYTES;
        if (pos + copied <= PHONEMIC_RING_BYTES)
        {
            RtlCopyMemory(dest, g_Ring + pos, copied);
        }
        else
        {
            ULONG first = PHONEMIC_RING_BYTES - pos;
            RtlCopyMemory(dest, g_Ring + pos, first);
            RtlCopyMemory(dest + first, g_Ring, copied - first);
        }
        g_RingFill -= copied;
    }
    KeReleaseSpinLock(&g_RingLock, oldIrql);
    return copied;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
ULONG PhonemicRingBufferedBytes(VOID)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_RingLock, &oldIrql);
    ULONG fill = g_RingFill;
    KeReleaseSpinLock(&g_RingLock, oldIrql);
    return fill;
}

VOID PhonemicRingCountUnderrun(VOID)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_RingLock, &oldIrql);
    g_Underruns++;
    KeReleaseSpinLock(&g_RingLock, oldIrql);
}

// ----------------------------- control device ------------------------------

static PDEVICE_OBJECT g_ControlDevice = NULL;
static UNICODE_STRING g_SymbolicLink;
static BOOLEAN g_InterfaceEnabled = FALSE;

/*
 * Расширение control-устройства (магия для диспетчера IOCTL).
 */
typedef struct _PHONEMIC_CTL_EXTENSION {
    ULONG Magic;
} PHONEMIC_CTL_EXTENSION, *PPHONEMIC_CTL_EXTENSION;

#define PHONEMIC_CTL_MAGIC 0x504D4354 // 'PMCT'

#pragma code_seg("PAGE")
NTSTATUS PhonemicControlDeviceCreate(_In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();

    if (g_ControlDevice != NULL) return STATUS_SUCCESS; // уже создано

    NTSTATUS status;
    UNICODE_STRING devName, sddl;
    RtlInitUnicodeString(&devName, PHONEMIC_DEVICE_NAME);
    RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;BU)");

    status = IoCreateDeviceSecure(
        DriverObject,
        sizeof(PHONEMIC_CTL_EXTENSION),
        &devName,
        FILE_DEVICE_PHONEMIC,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &sddl,
        NULL,
        &g_ControlDevice);
    if (!NT_SUCCESS(status)) return status;

    PPHONEMIC_CTL_EXTENSION ext = (PPHONEMIC_CTL_EXTENSION)g_ControlDevice->DeviceExtension;
    ext->Magic = PHONEMIC_CTL_MAGIC;
    g_ControlDevice->Flags |= DO_BUFFERED_IO;

    // символическая ссылка \\.\PhoneMic
    RtlInitUnicodeString(&g_SymbolicLink, PHONEMIC_SYMLINK_NAME);
    status = IoCreateSymbolicLink(&g_SymbolicLink, &devName);
    if (!NT_SUCCESS(status))
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL, "PhoneMic: symlink failed 0x%x\n", status);
        // не фатально: приложение найдёт устройство через интерфейс
    }

    // интерфейс устройства
    UNICODE_STRING interfaceName;
    status = IoRegisterDeviceInterface(
        g_ControlDevice,
        (LPGUID)&GUID_DEVINTERFACE_PHONEMIC,
        NULL,
        &interfaceName);
    if (NT_SUCCESS(status))
    {
        status = IoSetDeviceInterfaceState(&interfaceName, TRUE);
        if (NT_SUCCESS(status)) g_InterfaceEnabled = TRUE;
        RtlFreeUnicodeString(&interfaceName);
    }

    // кольцо данных
    status = PhonemicRingInit();
    if (!NT_SUCCESS(status))
    {
        IoDeleteDevice(g_ControlDevice);
        g_ControlDevice = NULL;
        return status;
    }

    return STATUS_SUCCESS;
}
#pragma code_seg()

VOID PhonemicControlDeviceCleanup(VOID)
{
    if (g_ControlDevice == NULL) return;
    if (g_InterfaceEnabled)
    {
        UNICODE_STRING interfaceName;
        // повторно регистрировать не обязательно; отключение делаем по имени
        UNICODE_STRING devName;
        RtlInitUnicodeString(&devName, PHONEMIC_DEVICE_NAME);
        UNREFERENCED_PARAMETER(interfaceName);
        UNREFERENCED_PARAMETER(devName);
    }
    if (g_Ring)
    {
        ExFreePoolWithTag(g_Ring, PHONEMIC_TAG_RING);
        g_Ring = NULL;
    }
    RtlInitUnicodeString(&g_SymbolicLink, PHONEMIC_SYMLINK_NAME);
    IoDeleteSymbolicLink(&g_SymbolicLink);
    IoDeleteDevice(g_ControlDevice);
    g_ControlDevice = NULL;
}

// ----------------------------- IOCTL dispatch ------------------------------

static NTSTATUS HandleGetState(_Out_ PVOID outBuf, _In_ ULONG outLen, _Out_ PULONG written)
{
    *written = 0;
    if (outLen < sizeof(PHONEMIC_STATE)) return STATUS_BUFFER_TOO_SMALL;
    PPHONEMIC_STATE st = (PPHONEMIC_STATE)outBuf;
    RtlZeroMemory(st, sizeof(PHONEMIC_STATE));
    st->Version = PHONEMIC_IOCTL_VERSION;
    st->Active = g_Active;
    st->Underruns = g_Underruns;
    st->Overflows = g_Overflows;
    st->BufferedBytes = PhonemicRingBufferedBytes();
    st->RingBytes = PHONEMIC_RING_BYTES;
    st->PeriodMicrosec = PHONEMIC_PERIOD_MICROSEC;
    st->SampleRate = PHONEMIC_SAMPLE_RATE;
    *written = sizeof(PHONEMIC_STATE);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS PhonemicDispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    // Control-устройство? Иначе — портам (KS).
    if (DeviceObject == NULL || DeviceObject->DeviceExtension == NULL ||
        ((PPHONEMIC_CTL_EXTENSION)DeviceObject->DeviceExtension)->Magic != PHONEMIC_CTL_MAGIC)
    {
        return PcDispatchIrp(DeviceObject, Irp);
    }

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG ioctl = irpSp->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen = irpSp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID outBuf = Irp->AssociatedIrp.SystemBuffer;
    PVOID inBuf = Irp->AssociatedIrp.SystemBuffer;

    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG written = 0;

    switch (ioctl)
    {
    case IOCTL_PHONEMIC_GET_VERSION:
        if (outLen >= sizeof(ULONG))
        {
            *(PULONG)outBuf = PHONEMIC_IOCTL_VERSION;
            written = sizeof(ULONG);
            status = STATUS_SUCCESS;
        }
        else status = STATUS_BUFFER_TOO_SMALL;
        break;

    case IOCTL_PHONEMIC_PUSH_SAMPLES:
    {
        if (inLen == 0 || inLen > 65536 || g_Ring == NULL)
        {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        KIRQL oldIrql;
        KeAcquireSpinLock(&g_RingLock, &oldIrql);
        ULONG accepted = 0;
        if (g_RingFill + inLen <= PHONEMIC_RING_BYTES)
        {
            ULONG pos = (g_RingWritePos + g_RingFill) % PHONEMIC_RING_BYTES;
            ULONG tail = min(inLen, PHONEMIC_RING_BYTES - pos);
            RtlCopyMemory(g_Ring + pos, inBuf, tail);
            if (inLen > tail)
                RtlCopyMemory(g_Ring, (PUCHAR)inBuf + tail, inLen - tail);
            g_RingFill += inLen;
            g_RingWritePos = (g_RingWritePos + inLen) % PHONEMIC_RING_BYTES;
            accepted = inLen;
        }
        else
        {
            // переполнение: принимаем частично
            ULONG space = PHONEMIC_RING_BYTES - g_RingFill;
            if (space > 0)
            {
                ULONG pos = (g_RingWritePos + g_RingFill) % PHONEMIC_RING_BYTES;
                ULONG tail = min(space, PHONEMIC_RING_BYTES - pos);
                RtlCopyMemory(g_Ring + pos, inBuf, tail);
                if (space > tail)
                    RtlCopyMemory(g_Ring, (PUCHAR)inBuf + tail, space - tail);
                g_RingFill += space;
                g_RingWritePos = (g_RingWritePos + space) % PHONEMIC_RING_BYTES;
                accepted = space;
            }
            g_Overflows++;
        }
        KeReleaseSpinLock(&g_RingLock, oldIrql);
        if (outLen >= sizeof(ULONG))
        {
            *(PULONG)outBuf = accepted;
            written = sizeof(ULONG);
        }
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_PHONEMIC_GET_STATE:
        status = HandleGetState(outBuf, outLen, &written);
        break;

    case IOCTL_PHONEMIC_SET_ACTIVE:
        if (inLen >= sizeof(ULONG))
        {
            g_Active = (*(PULONG)inBuf != 0) ? 1 : 0;
            status = STATUS_SUCCESS;
        }
        else status = STATUS_BUFFER_TOO_SMALL;
        break;

    case IOCTL_PHONEMIC_FLUSH:
        PhonemicRingReset();
        status = STATUS_SUCCESS;
        break;

    default:
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = written;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

_Use_decl_annotations_
NTSTATUS PhonemicDispatchCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    if (DeviceObject == NULL || DeviceObject->DeviceExtension == NULL ||
        ((PPHONEMIC_CTL_EXTENSION)DeviceObject->DeviceExtension)->Magic != PHONEMIC_CTL_MAGIC)
    {
        return PcDispatchIrp(DeviceObject, Irp);
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS PhonemicDispatchClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    if (DeviceObject == NULL || DeviceObject->DeviceExtension == NULL ||
        ((PPHONEMIC_CTL_EXTENSION)DeviceObject->DeviceExtension)->Magic != PHONEMIC_CTL_MAGIC)
    {
        return PcDispatchIrp(DeviceObject, Irp);
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}
