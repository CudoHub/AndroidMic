/*++
PhoneMic driver: адаптер (DriverEntry, AddDevice, StartDevice, регистрация
подустройств WaveRT + Topology, физическое соединение, control-устройство).
Модель: PortCls (sysvad-style InstallSubdevice).
--*/
#include "common.h"
#include "adapter.h"
#include "minwavert.h"
#include "mintopo.h"
#include "control.h"

#define MAX_MINIPORTS 2

// ------------------------------ AddDevice ----------------------------------

NTSTATUS AddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject
    )
{
    PAGED_CODE();
    return PcAddAdapterDevice(DriverObject, PhysicalDeviceObject, PhonemicStartDevice, MAX_MINIPORTS, 0);
}

// --------------------------- InstallSubdevice ------------------------------
// Создаёт порт + минипорт, инициализирует и регистрирует подустройство.
#pragma code_seg("PAGE")
static NTSTATUS InstallSubdevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList,
    _In_ GUID PortClassId,
    _In_ PCWSTR PortName,
    _In_ PFNCREATEMINIPORT MiniportCreate,
    _Out_opt_ PUNKNOWN* OutPortUnknown
    )
{
    PAGED_CODE();
    NTSTATUS ntStatus;
    PUNKNOWN unknownPort = NULL;
    PUNKNOWN unknownMiniport = NULL;
    IPort* port = NULL;

    ntStatus = PcNewPort(&unknownPort, PortClassId);
    if (!NT_SUCCESS(ntStatus)) goto Done;

    ntStatus = MiniportCreate(&unknownMiniport, NULL, NonPagedPoolNx, NULL);
    if (!NT_SUCCESS(ntStatus)) goto Done;

    ntStatus = unknownPort->QueryInterface(IID_PPV_ARGS(&port));
    if (!NT_SUCCESS(ntStatus)) goto Done;

    ntStatus = port->Init(DeviceObject, unknownMiniport, NULL, ResourceList, Irp);
    if (!NT_SUCCESS(ntStatus)) goto Done;

    ntStatus = PcRegisterSubdevice(DeviceObject, (PWSTR)PortName, unknownPort);
    if (!NT_SUCCESS(ntStatus)) goto Done;

    if (OutPortUnknown)
    {
        *OutPortUnknown = unknownPort;
        unknownPort = NULL; // передаём ссылку наружу
    }

Done:
    if (port) port->Release();
    if (unknownMiniport) unknownMiniport->Release();
    if (unknownPort) unknownPort->Release();
    return ntStatus;
}
#pragma code_seg()

// ------------------------------ StartDevice --------------------------------

#pragma code_seg("PAGE")
NTSTATUS PhonemicStartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList
    )
{
    PAGED_CODE();

    NTSTATUS ntStatus;
    PUNKNOWN unknownWave = NULL;
    PUNKNOWN unknownTopology = NULL;

    // 1. Wave: PortWaveRT + capture miniport
    ntStatus = InstallSubdevice(DeviceObject, Irp, ResourceList,
        CLSID_PortWaveRT, L"Wave", PhonemicCreateMiniportWaveRT, &unknownWave);
    if (!NT_SUCCESS(ntStatus)) goto Done;

    // 2. Topology: PortTopology + topo miniport
    ntStatus = InstallSubdevice(DeviceObject, Irp, ResourceList,
        CLSID_PortTopology, L"Topology", PhonemicCreateMiniportTopology, &unknownTopology);
    if (!NT_SUCCESS(ntStatus)) goto Done;

    // 3. Физическое соединение topology <-> wave.
    // Направление: источник данных в топологии (PIN_TOPO_WAVESINK) -> capture-пин wave.
    ntStatus = PcRegisterPhysicalConnection(
        DeviceObject,
        unknownTopology,
        PIN_TOPO_WAVESINK,
        unknownWave,
        PIN_WAVE_CAPTURE);
    if (!NT_SUCCESS(ntStatus))
    {
        // Не критично: capture работает и без связи с топологией
        // (громкость/мьют Windows применит штатными APO).
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
            "PhoneMic: physical connection failed 0x%x (non-fatal)\n", ntStatus);
        ntStatus = STATUS_SUCCESS;
    }

    // 4. Контрольное устройство (IOCTL-кольцо для WinUI-приложения)
    ntStatus = PhonemicControlDeviceCreate(DeviceObject->DriverObject);
    if (!NT_SUCCESS(ntStatus)) goto Done;

Done:
    if (!NT_SUCCESS(ntStatus))
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "PhoneMic: StartDevice failed 0x%x\n", ntStatus);
    }
    if (unknownWave) unknownWave->Release();
    if (unknownTopology) unknownTopology->Release();
    return ntStatus;
}
#pragma code_seg()

// ------------------------------ DriverEntry --------------------------------

extern "C" DRIVER_UNLOAD PhonemicUnload;

_Use_decl_annotations_
NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS ntStatus = PcInitializeAdapterDriver(DriverObject, RegistryPath, AddDevice);
    if (!NT_SUCCESS(ntStatus)) return ntStatus;

    // Свой диспетчер DEVICE_CONTROL: IOCTL control-устройства, остальное — PcDispatchIrp.
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = PhonemicDispatchDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = PhonemicDispatchCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = PhonemicDispatchClose;

    DriverObject->DriverUnload = PhonemicUnload;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "PhoneMic: DriverEntry OK\n");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID PhonemicUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    PhonemicControlDeviceCleanup();
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "PhoneMic: unload\n");
}
