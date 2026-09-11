/*++
PhoneMic driver: поток WaveRT (IMiniportWaveRTStream + notification).
Буфер, DPC-таймер, позиция/часы, события уведомлений, подтягивание из кольца.
--*/
#pragma once
#include "common.h"

// состояние shared-страницы регистров (position/clock register)
typedef struct _PHONEMIC_REGISTERS {
    volatile ULONG64 ClockQpc;      // QPC при последнем тике
    volatile ULONG  ClockPosition;  // позиция (байты, циклическая)
    volatile ULONG  Position;       // position register (ULONG, циклическая)
    volatile ULONG  Padding;
} PHONEMIC_REGISTERS, *PPHONEMIC_REGISTERS;

class CMiniportWaveRTStream :
    public IMiniportWaveRTStream,
    public IMiniportWaveRTStreamNotification,
    public CUnknown
{
public:
    DECLARE_USING_UNKNOWN()

    CMiniportWaveRTStream(_In_ PUNKNOWN OuterUnknown);
    ~CMiniportWaveRTStream();

    // IMiniportWaveRTStream
    IMP_IMiniportWaveRTStream(SetState);
    IMP_IMiniportWaveRTStream(GetClockRegister);
    IMP_IMiniportWaveRTStream(GetPositionRegister);

    // IMiniportWaveRTStreamNotification
    IMP_IMiniportWaveRTStreamNotification(AllocateBufferWithNotification);
    IMP_IMiniportWaveRTStreamNotification(FreeBufferWithNotification);

    // локальные методы
    NTSTATUS Init(_In_ CMiniportWaveRT* parent, _In_ PPORTWAVERTSTREAM portStream, _In_ BOOLEAN capture);
    NTSTATUS AllocateWaveRtBuffer(_In_ ULONG requestedSize);
    VOID FreeWaveRtBuffer();
    VOID DpcTick();   // вызывается из DPC: копирование из кольца + позиция + события

    // доступ для property-обработчиков
    PVOID GetUserAddress() { return m_UserAddress; }
    ULONG GetBufferSize() { return m_BufferSize; }
    volatile VOID* GetPositionRegisterPtr() { return &m_Registers->Position; }
    volatile VOID* GetClockRegisterPtr() { return &m_Registers->ClockQpc; }
    NTSTATUS AddNotificationEvent(_In_ PKEVENT event);
    NTSTATUS RemoveNotificationEvent(_In_ HANDLE userHandle);

    // фабрика
    static NTSTATUS Create(
        _Outptr_ PUNKNOWN* Unknown,
        _In_ REFCLSID Clsid,
        _In_ POOL_TYPE PoolType,
        _In_ PUNKNOWN OuterUnknown,
        _In_ CMiniportWaveRT* parent,
        _In_ PPORTWAVERTSTREAM portStream,
        _In_ BOOLEAN capture);

protected:
    CMiniportWaveRT* m_Parent = nullptr;
    PPORTWAVERTSTREAM m_PortStream = nullptr;
    BOOLEAN m_Capture = TRUE;
    KSSTATE m_State = KSSTATE_STOP;

    // WaveRT-буфер
    PMDL m_BufferMdl = nullptr;
    PVOID m_SystemAddress = nullptr;   // системный VA
    PVOID m_UserAddress = nullptr;     // замапленный в user
    ULONG m_BufferSize = 0;
    BOOLEAN m_FromPool = FALSE;

    // позиция
    PPHONEMIC_REGISTERS m_Registers = nullptr;  // non-cached страница
    ULONG64 m_TotalPosition = 0;                // суммарные байты с RUN

    // события уведомлений (до 4)
    struct {
        PKEVENT Event;
        HANDLE UserHandle;
    } m_NotificationEvents[4];
    ULONG m_EventCount = 0;
    ULONG m_NotificationCount = 0;

    // таймер DPC
    KTIMER m_Timer;
    KDPC m_Dpc;
    BOOLEAN m_TimerArmed = FALSE;
    ULONG m_PreviousTimerResolution = 0;
};
