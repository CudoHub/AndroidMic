/*++
PhoneMic driver: реализация потока WaveRT.
Аудио цикл: DPC каждые 5 мс копирует PCM из кольца (IOCTL) в WaveRT-циклический
буфер, двигает position/clock-регистры, сигнализирует notification events.
Точку выделения буфера держим в ОДНОЙ функции (AllocateWaveRtBuffer) — сигнатуры
IPortWaveRTStream различаются между версиями WDK, при сборке правится только она
(см. docs/DRIVER_BUILD.md, раздел «Известные правки»).
--*/
#include "common.h"
#include "adapter.h"
#include "minwavert.h"
#include "minwavertstream.h"
#include "control.h"

extern "C" VOID PhonemicStreamDpc(_In_ PKDPC Dpc, _In_opt_ PVOID context, _In_opt_ PVOID sysArg1, _In_opt_ PVOID sysArg2);

// =========================== CMiniportWaveRTStream ==========================

CMiniportWaveRTStream::CMiniportWaveRTStream(_In_ PUNKNOWN OuterUnknown) :
    CUnknown("MiniportWaveRTStream", OuterUnknown)
{
    KeInitializeTimer(&m_Timer);
    KeInitializeDpc(&m_Dpc, PhonemicStreamDpc, this);
    RtlZeroMemory(m_NotificationEvents, sizeof(m_NotificationEvents));
}

CMiniportWaveRTStream::~CMiniportWaveRTStream()
{
    FreeWaveRtBuffer();
    if (m_PortStream) m_PortStream->Release();
    if (m_Parent) m_Parent->SetStream(nullptr);

    // поток умирает — считаем захват неактивным
    InterlockedExchange(&g_CaptureStreamActive, 0);
    if (m_PreviousTimerResolution)
    {
        ExSetTimerResolution(0, FALSE);
        m_PreviousTimerResolution = 0;
    }
}

NTSTATUS CMiniportWaveRTStream::Create(
    _Outptr_ PUNKNOWN* Unknown,
    _In_ REFCLSID Clsid,
    _In_ POOL_TYPE PoolType,
    _In_ PUNKNOWN OuterUnknown,
    _In_ CMiniportWaveRT* parent,
    _In_ PPORTWAVERTSTREAM portStream,
    _In_ BOOLEAN capture)
{
    UNREFERENCED_PARAMETER(Clsid);
    UNREFERENCED_PARAMETER(PoolType);
    UNREFERENCED_PARAMETER(capture);

    CMiniportWaveRTStream* obj = new (NonPagedPoolNx, PHONEMIC_TAG_GEN) CMiniportWaveRTStream(OuterUnknown);
    if (obj == nullptr) return STATUS_INSUFFICIENT_RESOURCES;
    obj->AddRef();
    obj->m_Parent = parent;
    obj->m_PortStream = portStream;
    portStream->AddRef();
    *Unknown = (PUNKNOWN)(IMiniportWaveRTStream*)obj;
    return STATUS_SUCCESS;
}

NTSTATUS CMiniportWaveRTStream::Init(
    _In_ CMiniportWaveRT* parent,
    _In_ PPORTWAVERTSTREAM portStream,
    _In_ BOOLEAN capture)
{
    UNREFERENCED_PARAMETER(portStream);
    PAGED_CODE();
    m_Parent = parent;
    m_Capture = capture;

    // non-cached страница регистров (position register обязан быть вне кэша)
    m_Registers = (PPHONEMIC_REGISTERS)MmAllocateNonCachedMemory(sizeof(PHONEMIC_REGISTERS));
    if (m_Registers == nullptr) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(m_Registers, sizeof(PHONEMIC_REGISTERS));

    PhonemicRingReset();
    return STATUS_SUCCESS;
}

// --------------------------- выделение буфера ------------------------------
/*
 * ВНИМАНИЕ (WDK-совместимость): ниже — единственное место, где используется
 * IPortWaveRTStream::AllocateContiguousPhysicalMemory / MapAllocatedPagesToUserMode.
 * Если в вашей версии WDK сигнатуры иные — поправьте ТОЛЬКО этот блок:
 *   вариант A (порт-хелперы, как в sysvad):
 *       m_PortStream->AllocateContiguousPhysicalMemory(size, &phys, &sysVa, &mdl);
 *       userVa = m_PortStream->MapAllocatedPagesToUserMode(mdl, MmCached);
 *   вариант B (без хелперов порта, включён по умолчанию): свой пул + MDL,
 *       user-маппинг делает PortCls при обработке KSPROPERTY_RTAUDIO_BUFFER.
 */
NTSTATUS CMiniportWaveRTStream::AllocateWaveRtBuffer(_In_ ULONG requestedSize)
{
    PAGED_CODE();
    if (m_BufferMdl != nullptr) return STATUS_SUCCESS; // уже есть

    ULONG size = max(requestedSize, PHONEMIC_MIN_WAVERT_BUFFER);
    size = min(size, PHONEMIC_MAX_WAVERT_BUFFER);

    PHYSICAL_ADDRESS low = { 0 }, high = { 0 }, skip = { 0 };
    high.QuadPart = (LONGLONG)(~0ULL);
    m_SystemAddress = MmAllocateContiguousMemorySpecifyCache(
        size, low, high, skip, MmCached);
    if (m_SystemAddress == nullptr) return STATUS_INSUFFICIENT_RESOURCES;

    m_BufferMdl = IoAllocateMdl(m_SystemAddress, size, FALSE, FALSE, nullptr);
    if (m_BufferMdl == nullptr)
    {
        MmFreeContiguousMemorySpecifyCache(m_SystemAddress, size, MmCached);
        m_SystemAddress = nullptr;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    MmBuildMdlForNonPagedPool(m_BufferMdl);

    // маппинг в пользовательский процесс (звуковой движок) — вызов происходит
    // в его контексте из property-handler
    m_UserAddress = MmMapLockedPagesSpecifyCache(
        m_BufferMdl, UserMode, MmCached, nullptr, FALSE, NormalPagePriority);
    if (m_UserAddress == nullptr)
    {
        IoFreeMdl(m_BufferMdl);
        m_BufferMdl = nullptr;
        MmFreeContiguousMemorySpecifyCache(m_SystemAddress, size, MmCached);
        m_SystemAddress = nullptr;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    m_BufferSize = size;
    m_FromPool = TRUE;
    RtlZeroMemory(m_SystemAddress, m_BufferSize);
    return STATUS_SUCCESS;
}

VOID CMiniportWaveRTStream::FreeWaveRtBuffer()
{
    if (m_UserAddress && m_BufferMdl)
    {
        MmUnmapLockedPages(m_UserAddress, m_BufferMdl);
        m_UserAddress = nullptr;
    }
    if (m_BufferMdl)
    {
        IoFreeMdl(m_BufferMdl);
        m_BufferMdl = nullptr;
    }
    if (m_SystemAddress)
    {
        MmFreeContiguousMemorySpecifyCache(m_SystemAddress, m_BufferSize, MmCached);
        m_SystemAddress = nullptr;
    }
    m_BufferSize = 0;
}

// ------------------------------ SetState -----------------------------------

IMP_IMiniportWaveRTStream::SetState(_In_ KSSTATE State)
{
    PAGED_CODE();

    switch (State)
    {
    case KSSTATE_RUN:
        if (m_State != KSSTATE_RUN)
        {
            InterlockedExchange(&g_CaptureStreamActive, 1);
            // поднимаем разрешение таймера до ~1 мс, DPC сам переармится на 5 мс
            m_PreviousTimerResolution = ExSetTimerResolution(10000 /* 1 мс */, TRUE);
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(PHONEMIC_PERIOD_MICROSEC * 10); // 5 мс, отрицательное = относительно
            KeSetTimer(&m_Timer, due, &m_Dpc);
            m_TimerArmed = TRUE;
        }
        break;

    case KSSTATE_STOP:
        if (m_TimerArmed)
        {
            KeCancelTimer(&m_Timer);
            m_TimerArmed = FALSE;
        }
        InterlockedExchange(&g_CaptureStreamActive, 0);
        ExSetTimerResolution(0, FALSE);
        m_PreviousTimerResolution = 0;
        PhonemicRingReset();
        m_TotalPosition = 0;
        break;

    case KSSTATE_PAUSE:
    case KSSTATE_ACQUIRE:
    default:
        break;
    }

    m_State = State;
    return STATUS_SUCCESS;
}

// ------------------------------ регистры -----------------------------------

IMP_IMiniportWaveRTStream::GetClockRegister(_Out_ PKSRTAUDIO_HWREGISTER Register)
{
    Register->Register = (PVOID)&m_Registers->ClockQpc;
    Register->Width = 64;
    Register->Precision = 64;
    Register->Flags = KSRTAUDIO_HWREGISTER_TIME | KSRTAUDIO_HWREGISTER_POSITION;
    return STATUS_SUCCESS;
}

IMP_IMiniportWaveRTStream::GetPositionRegister(_Out_ PKSRTAUDIO_HWREGISTER Register)
{
    Register->Register = (PVOID)&m_Registers->Position;
    Register->Width = 32;
    Register->Precision = 32;
    Register->Flags = KSRTAUDIO_HWREGISTER_POSITION;
    return STATUS_SUCCESS;
}

// ---------------------- notification buffer (IMiniport...Notification) -----

IMP_IMiniportWaveRTStreamNotification::AllocateBufferWithNotification(
    _In_ ULONG RequestedSize,
    _Deref_out_ PMDL* AudioBufferMdl,
    _Deref_out_ PVOID* AudioBufferVirtualAddress,
    _Deref_out_ ULONG* ActualSize,
    _Out_ BOOLEAN* FromPool,
    _Out_ BOOLEAN* Cached)
{
    UNREFERENCED_PARAMETER(Cached);
    PAGED_CODE();

    NTSTATUS ntStatus = AllocateWaveRtBuffer(RequestedSize);
    if (!NT_SUCCESS(ntStatus)) return ntStatus;

    *AudioBufferMdl = m_BufferMdl;
    *AudioBufferVirtualAddress = m_SystemAddress;
    *ActualSize = m_BufferSize;
    *FromPool = TRUE;
    return STATUS_SUCCESS;
}

IMP_IMiniportWaveRTStreamNotification::FreeBufferWithNotification()
{
    PAGED_CODE();
    FreeWaveRtBuffer();
    return STATUS_SUCCESS;
}

// ------------------------------ DPC ----------------------------------------

extern "C" VOID PhonemicStreamDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    CMiniportWaveRTStream* stream = (CMiniportWaveRTStream*)Context;
    if (stream == nullptr) return;
    stream->DpcTick();
}

VOID CMiniportWaveRTStream::DpcTick()
{
    if (m_State != KSSTATE_RUN || m_SystemAddress == nullptr)
    {
        // переармить и выйти
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(PHONEMIC_PERIOD_MICROSEC * 10);
        KeSetTimer(&m_Timer, due, &m_Dpc);
        return;
    }

    ULONG periodBytes = PHONEMIC_PERIOD_BYTES; // 480 Б = 5 мс

    // 1. копируем из кольца в WaveRT по кругу
    ULONG pos = (ULONG)(m_TotalPosition % m_BufferSize);
    ULONG tail = m_BufferSize - pos;
    ULONG chunk = min(periodBytes, tail);
    PUCHAR dst = (PUCHAR)m_SystemAddress + pos;

    ULONG copied = PhonemicRingPull(dst, chunk);
    if (copied < chunk) RtlZeroMemory(dst + copied, chunk - copied); // недобор — тишина
    if (copied < periodBytes && m_Parent != nullptr)
    {
        PhonemicRingCountUnderrun();
    }
    if (chunk < periodBytes)
    {
        // дошли до конца буфера — копируем продолжение в начало
        ULONG rest = periodBytes - chunk;
        PUCHAR dst2 = (PUCHAR)m_SystemAddress;
        ULONG copied2 = PhonemicRingPull(dst2, rest);
        if (copied2 < rest) RtlZeroMemory(dst2 + copied2, rest - copied2);
    }

    m_TotalPosition += periodBytes;

    // 2. регистры
    ULONG64 qpc = KeQueryInterruptTimePrecise(nullptr);
    if (qpc == 0) qpc = KeQueryInterruptTime();
    m_Registers->ClockQpc = qpc;
    m_Registers->ClockPosition = (ULONG)(m_TotalPosition % m_BufferSize);
    m_Registers->Position = (ULONG)(m_TotalPosition % m_BufferSize);

    // 3. события уведомлений
    for (ULONG i = 0; i < m_EventCount; i++)
    {
        if (m_NotificationEvents[i].Event)
            KeSetEvent(m_NotificationEvents[i].Event, IO_NO_INCREMENT, FALSE);
    }

    // 4. переармим
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(PHONEMIC_PERIOD_MICROSEC * 10);
    KeSetTimer(&m_Timer, due, &m_Dpc);
}

// ------------------------------ события ------------------------------------

NTSTATUS CMiniportWaveRTStream::AddNotificationEvent(_In_ PKEVENT event)
{
    if (m_EventCount >= RTL_NUMBER_OF(m_NotificationEvents)) return STATUS_INSUFFICIENT_RESOURCES;
    for (ULONG i = 0; i < m_EventCount; i++)
        if (m_NotificationEvents[i].Event == event) return STATUS_SUCCESS; // уже есть
    m_NotificationEvents[m_EventCount].Event = event;
    m_NotificationEvents[m_EventCount].UserHandle = nullptr;
    m_EventCount++;
    return STATUS_SUCCESS;
}

NTSTATUS CMiniportWaveRTStream::RemoveNotificationEvent(_In_ HANDLE userHandle)
{
    // сопоставляем пользовательский хэндл через второй ObReference
    PKEVENT target = nullptr;
    NTSTATUS status = ObReferenceObjectByHandle(
        userHandle, EVENT_MODIFY_STATE, *ExEventObjectType, UserMode, (PVOID*)&target, nullptr);
    if (!NT_SUCCESS(status)) return status;

    NTSTATUS result = STATUS_NOT_FOUND;
    for (ULONG i = 0; i < m_EventCount; i++)
    {
        if (m_NotificationEvents[i].Event == target)
        {
            ObDereferenceObject(m_NotificationEvents[i].Event);
            // сдвигаем хвост
            for (ULONG j = i; j + 1 < m_EventCount; j++)
                m_NotificationEvents[j] = m_NotificationEvents[j + 1];
            m_EventCount--;
            RtlZeroMemory(&m_NotificationEvents[m_EventCount], sizeof(m_NotificationEvents[0]));
            result = STATUS_SUCCESS;
            break;
        }
    }
    ObDereferenceObject(target);
    return result;
}
