/*++
PhoneMic driver: реализация потока WaveRT.
Аудио цикл: DPC каждые 5 мс копирует PCM из кольца (IOCTL) в WaveRT-циклический
буфер, двигает position/clock-регистры, сигнализирует notification events.
Реализованы ВСЕ чистые методы IMiniportWaveRTStream и
IMiniportWaveRTStreamNotification (по portcls.h 26100).
--*/
#include "common.h"
#include "adapter.h"
#include "minwavert.h"
#include "minwavertstream.h"
#include "control.h"

extern "C" VOID PhonemicStreamDpc(_In_ PKDPC Dpc, _In_opt_ PVOID context, _In_opt_ PVOID sysArg1, _In_opt_ PVOID sysArg2);

// =========================== CMiniportWaveRTStream ==========================

CMiniportWaveRTStream::CMiniportWaveRTStream(_In_ PUNKNOWN OuterUnknown) :
    CUnknown(OuterUnknown)
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

    // снимаем наши ссылки на event-объекты
    for (ULONG i = 0; i < m_EventCount; i++)
    {
        if (m_NotificationEvents[i].Event)
            ObDereferenceObject(m_NotificationEvents[i].Event);
    }
    RtlZeroMemory(m_NotificationEvents, sizeof(m_NotificationEvents));
    m_EventCount = 0;

    // поток умирает — считаем захват неактивным
    InterlockedExchange(&g_CaptureStreamActive, 0);
    if (m_PreviousTimerResolution)
    {
        ExSetTimerResolution(0, FALSE);
        m_PreviousTimerResolution = 0;
    }
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::NonDelegatingQueryInterface(_In_ REFIID Interface, _COM_Outptr_ PVOID* Object)
{
    ASSERT(Object);
    if (!Object) return STATUS_INVALID_PARAMETER;

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = (PVOID)(PUNKNOWN)(IMiniportWaveRTStream*)this;
        ((PUNKNOWN)*Object)->AddRef();
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStream))
    {
        *Object = (PVOID)(IMiniportWaveRTStream*)this;
        ((PUNKNOWN)(IMiniportWaveRTStream*)this)->AddRef();
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStreamNotification))
    {
        *Object = (PVOID)(IMiniportWaveRTStreamNotification*)this;
        ((PUNKNOWN)(IMiniportWaveRTStreamNotification*)this)->AddRef();
    }
    else
    {
        *Object = nullptr;
        return CUnknown::NonDelegatingQueryInterface(Interface, Object);
    }
    return STATUS_SUCCESS;
}

NTSTATUS CMiniportWaveRTStream::Create(
    _Out_ CMiniportWaveRTStream** Stream,
    _In_ CMiniportWaveRT* parent,
    _In_ PPORTWAVERTSTREAM portStream,
    _In_ BOOLEAN capture,
    _In_ PUNKNOWN OuterUnknown)
{
    if (!Stream || !portStream) return STATUS_INVALID_PARAMETER;

    CMiniportWaveRTStream* obj =
        new (NonPagedPoolNx, PHONEMIC_TAG_GEN) CMiniportWaveRTStream(OuterUnknown);
    if (obj == nullptr) return STATUS_INSUFFICIENT_RESOURCES;
    obj->AddRef();
    obj->m_Parent = parent;
    obj->m_PortStream = portStream;
    obj->m_Capture = capture;
    portStream->AddRef();
    *Stream = obj;
    return STATUS_SUCCESS;
}

NTSTATUS CMiniportWaveRTStream::Init(
    _In_ CMiniportWaveRT* parent,
    _In_ PPORTWAVERTSTREAM portStream,
    _In_ BOOLEAN capture)
{
    UNREFERENCED_PARAMETER(parent);
    UNREFERENCED_PARAMETER(portStream);
    UNREFERENCED_PARAMETER(capture);
    PAGED_CODE();

    // non-cached страница регистров (position register обязан быть вне кэша)
    m_Registers = (PPHONEMIC_REGISTERS)MmAllocateNonCachedMemory(sizeof(PHONEMIC_REGISTERS));
    if (m_Registers == nullptr) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(m_Registers, sizeof(PHONEMIC_REGISTERS));

    PhonemicRingReset();
    return STATUS_SUCCESS;
}

// --------------------------- выделение буфера ------------------------------

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

// ---------------------- IMiniportWaveRTStream -------------------------------

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::SetFormat(_In_ PKSDATAFORMAT DataFormat)
{
    PAGED_CODE();
    if (DataFormat == nullptr) return STATUS_INVALID_PARAMETER;

    // движок работает в фикс-формате 48к/16/mono — остальное отклоняем
    if (DataFormat->FormatSize >= sizeof(KSDATAFORMAT_WAVEFORMATEX) &&
        IsEqualGUIDAligned(DataFormat->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) &&
        IsEqualGUIDAligned(DataFormat->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
    {
        PWAVEFORMATEX wf = &((PKSDATAFORMAT_WAVEFORMATEX)DataFormat)->WaveFormatEx;
        if (wf->wFormatTag == WAVE_FORMAT_PCM &&
            wf->nChannels == 1 &&
            wf->wBitsPerSample == 16 &&
            wf->nSamplesPerSec == PHONEMIC_SAMPLE_RATE)
        {
            return STATUS_SUCCESS;
        }
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::SetState(_In_ KSSTATE State)
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

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::GetPosition(_Out_ PKSAUDIO_POSITION Position)
{
    PAGED_CODE();
    ASSERT(Position);
    if (!Position) return STATUS_INVALID_PARAMETER;
    ULONG pos = (ULONG)(m_TotalPosition % max(m_BufferSize, 1));
    Position->PlayOffset = pos;
    Position->WriteOffset = pos;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::AllocateAudioBuffer(
    _In_ ULONG RequestedSize,
    _Out_ PMDL* AudioBufferMdl,
    _Out_ ULONG* ActualSize,
    _Out_ ULONG* OffsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE* CacheType
    )
{
    PAGED_CODE();
    ASSERT(AudioBufferMdl && ActualSize && OffsetFromFirstPage && CacheType);
    if (!AudioBufferMdl || !ActualSize || !OffsetFromFirstPage || !CacheType)
        return STATUS_INVALID_PARAMETER;

    NTSTATUS ntStatus = AllocateWaveRtBuffer(RequestedSize);
    if (!NT_SUCCESS(ntStatus)) return ntStatus;

    *AudioBufferMdl = m_BufferMdl;
    *ActualSize = m_BufferSize;
    *OffsetFromFirstPage = 0;
    *CacheType = MmCached;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(VOID)
CMiniportWaveRTStream::FreeAudioBuffer(
    _In_opt_ PMDL AudioBufferMdl,
    _In_ ULONG BufferSize
    )
{
    UNREFERENCED_PARAMETER(AudioBufferMdl);
    UNREFERENCED_PARAMETER(BufferSize);
    PAGED_CODE();
    FreeWaveRtBuffer();
}

// IMiniportWaveRTStream::GetHWLatency возвращает VOID (portcls.h 26100,
// STDMETHODIMP_(VOID)), а не NTSTATUS
STDMETHODIMP_(VOID)
CMiniportWaveRTStream::GetHWLatency(_Out_ KSRTAUDIO_HWLATENCY* hwLatency)
{
    PAGED_CODE();
    ASSERT(hwLatency);
    if (!hwLatency) return;
    hwLatency->FifoSize = 0;
    hwLatency->ChipsetDelay = 0;
    hwLatency->CodecDelay = 0;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::GetPositionRegister(_Out_ KSRTAUDIO_HWREGISTER* Register)
{
    PAGED_CODE();
    ASSERT(Register && m_Registers);
    if (!Register || !m_Registers) return STATUS_INVALID_PARAMETER;
    Register->Register = (PVOID)&m_Registers->Position;
    Register->Width = 32;
    Register->Numerator = 1;
    Register->Denominator = 1;
    Register->Accuracy = 0;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::GetClockRegister(_Out_ KSRTAUDIO_HWREGISTER* Register)
{
    PAGED_CODE();
    ASSERT(Register && m_Registers);
    if (!Register || !m_Registers) return STATUS_INVALID_PARAMETER;
    Register->Register = (PVOID)&m_Registers->ClockQpc;
    Register->Width = 64;
    Register->Numerator = 1;
    Register->Denominator = 1;
    Register->Accuracy = 0;
    return STATUS_SUCCESS;
}

// ---------------- IMiniportWaveRTStreamNotification -------------------------

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::AllocateBufferWithNotification(
    _In_ ULONG NotificationCount,
    _In_ ULONG RequestedSize,
    _Out_ PMDL* AudioBufferMdl,
    _Out_ ULONG* ActualSize,
    _Out_ ULONG* OffsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE* CacheType
    )
{
    PAGED_CODE();
    ASSERT(AudioBufferMdl && ActualSize && OffsetFromFirstPage && CacheType);
    if (!AudioBufferMdl || !ActualSize || !OffsetFromFirstPage || !CacheType)
        return STATUS_INVALID_PARAMETER;

    NTSTATUS ntStatus = AllocateWaveRtBuffer(RequestedSize);
    if (!NT_SUCCESS(ntStatus)) return ntStatus;

    if (NotificationCount) m_NotificationCount = NotificationCount;
    *AudioBufferMdl = m_BufferMdl;
    *ActualSize = m_BufferSize;
    *OffsetFromFirstPage = 0;
    *CacheType = MmCached;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(VOID)
CMiniportWaveRTStream::FreeBufferWithNotification(
    _In_ PMDL AudioBufferMdl,
    _In_ ULONG BufferSize
    )
{
    UNREFERENCED_PARAMETER(AudioBufferMdl);
    UNREFERENCED_PARAMETER(BufferSize);
    PAGED_CODE();
    m_NotificationCount = 0;
    FreeWaveRtBuffer();
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::RegisterNotificationEvent(_In_ PKEVENT NotificationEvent)
{
    PAGED_CODE();
    if (!NotificationEvent) return STATUS_INVALID_PARAMETER;
    // порт сохраняет свою ссылку; заводим собственную на время жизни потока
    ObReferenceObject(NotificationEvent);
    NTSTATUS ntStatus = AddNotificationEvent(NotificationEvent);
    if (!NT_SUCCESS(ntStatus)) ObDereferenceObject(NotificationEvent);
    return ntStatus;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::UnregisterNotificationEvent(_In_ PKEVENT NotificationEvent)
{
    PAGED_CODE();
    if (!NotificationEvent) return STATUS_INVALID_PARAMETER;

    for (ULONG i = 0; i < m_EventCount; i++)
    {
        if (m_NotificationEvents[i].Event == NotificationEvent)
        {
            ObDereferenceObject(m_NotificationEvents[i].Event);
            for (ULONG j = i; j + 1 < m_EventCount; j++)
                m_NotificationEvents[j] = m_NotificationEvents[j + 1];
            m_EventCount--;
            RtlZeroMemory(&m_NotificationEvents[m_EventCount], sizeof(m_NotificationEvents[0]));
            return STATUS_SUCCESS;
        }
    }
    return STATUS_NOT_FOUND;
}
#pragma code_seg()

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
    ULONGLONG qpcTimeStamp = 0;
    (VOID)KeQueryInterruptTimePrecise(&qpcTimeStamp);
    m_Registers->ClockQpc = qpcTimeStamp;
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

// Принимает владение ссылкой на event (вызвавший уже ObReferenceObject)
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

// Путь из property-обработчика: хэндл -> объект, сверяем по объекту
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
