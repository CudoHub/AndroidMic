/*++
PhoneMic driver: обработчики KSPROPSETID_RtAudio (буфер, регистры, события).
Структуры — из ksmedia.h; вход/выход парсим защитно (по длине).
--*/
#include "common.h"
#include "adapter.h"
#include "minwavert.h"
#include "minwavertstream.h"

// контекст: MajorTarget = минипорт, MinorTarget = PPORTWAVERTSTREAM (поток порта)
static CMiniportWaveRT* GetMiniport(PPCPROPERTY_REQUEST req)
{
    return (CMiniportWaveRT*)req->MajorTarget;
}

static CMiniportWaveRTStream* GetStream(PPCPROPERTY_REQUEST req)
{
    CMiniportWaveRT* miniport = GetMiniport(req);
    return miniport ? miniport->GetStream() : nullptr;
}

// универсальный ответ статическим значением
static NTSTATUS Reply(PPCPROPERTY_REQUEST req, const void* data, ULONG size)
{
    if (req->ValueSize == 0)
    {
        req->ValueSize = size;   // запрос размера
        return STATUS_SUCCESS;
    }
    if (req->ValueSize < size) return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(req->Value, data, size);
    req->ValueSize = size;
    return STATUS_SUCCESS;
}

// --- KSPROPERTY_RTAUDIO_BUFFER / BUFFER_WITH_NOTIFICATION ---

static NTSTATUS HandleRtAudioBuffer(PPCPROPERTY_REQUEST req, BOOLEAN withNotification)
{
    CMiniportWaveRTStream* stream = GetStream(req);
    if (stream == nullptr) return STATUS_INVALID_DEVICE_REQUEST;

    // вход: KSRTAUDIO_BUFFER_PROPERTY {Property, BaseAddress, RequestedBufferSize}
    ULONG requestedSize = PHONEMIC_PERIOD_BYTES * 8; // разумный дефолт (40 мс)
    ULONG inputSize = req->Irp ? IoGetCurrentIrpStackLocation(req->Irp)->Parameters.DeviceIoControl.InputBufferLength : 0;
    if (inputSize >= sizeof(KSRTAUDIO_BUFFER_PROPERTY))
    {
        PKSRTAUDIO_BUFFER_PROPERTY bp = (PKSRTAUDIO_BUFFER_PROPERTY)req->Irp->AssociatedIrp.SystemBuffer;
        if (bp && bp->RequestedBufferSize > 0)
            requestedSize = bp->RequestedBufferSize;
    }
    UNREFERENCED_PARAMETER(withNotification);
    // NotificationCount приходит во ВХОДНОЙ KSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION;
    // ОТВЕТ для обоих свойств — KSRTAUDIO_BUFFER (структуры *_WITH_NOTIFICATION
    // для ответа в ksmedia.h нет)

    NTSTATUS status = stream->AllocateWaveRtBuffer(requestedSize);
    if (!NT_SUCCESS(status)) return status;

    PVOID userVa = stream->GetUserAddress();
    ULONG actual = stream->GetBufferSize();
    if (userVa == nullptr) return STATUS_UNSUCCESSFUL;

    KSRTAUDIO_BUFFER out;
    out.BufferAddress = userVa;
    out.ActualBufferSize = actual;
    out.CallMemoryBarrier = FALSE;   // кэшируемая память
    return Reply(req, &out, sizeof(out));
}

// --- KSPROPERTY_RTAUDIO_HWLATENCY ---

static NTSTATUS HandleRtAudioHwLatency(PPCPROPERTY_REQUEST req)
{
    KSRTAUDIO_HWLATENCY out;
    out.FifoSize = 0;
    out.ChipsetDelay = 0;
    out.CodecDelay = 0;
    return Reply(req, &out, sizeof(out));
}

// --- KSPROPERTY_RTAUDIO_POSITIONREGISTER ---

static NTSTATUS HandleRtAudioPositionRegister(PPCPROPERTY_REQUEST req)
{
    CMiniportWaveRTStream* stream = GetStream(req);
    if (stream == nullptr) return STATUS_INVALID_DEVICE_REQUEST;
    // значение KSPROPERTY_RTAUDIO_POSITIONREGISTER = KSRTAUDIO_HWREGISTER
    KSRTAUDIO_HWREGISTER out;
    out.Register = (PVOID)stream->GetPositionRegisterPtr();
    out.Width = 32;
    out.Numerator = 1;
    out.Denominator = 1;
    out.Accuracy = 0;
    return Reply(req, &out, sizeof(out));
}

// --- KSPROPERTY_RTAUDIO_CLOCKREGISTER ---

static NTSTATUS HandleRtAudioClockRegister(PPCPROPERTY_REQUEST req)
{
    CMiniportWaveRTStream* stream = GetStream(req);
    if (stream == nullptr) return STATUS_INVALID_DEVICE_REQUEST;
    KSRTAUDIO_HWREGISTER out;
    out.Register = (PVOID)stream->GetClockRegisterPtr();
    out.Width = 64;
    out.Numerator = 1;
    out.Denominator = 1;
    out.Accuracy = 0;
    return Reply(req, &out, sizeof(out));
}

// --- REGISTER/UNREGISTER_NOTIFICATION_EVENT ---

static NTSTATUS HandleNotificationEvent(PPCPROPERTY_REQUEST req, BOOLEAN registerEvent)
{
    CMiniportWaveRTStream* stream = GetStream(req);
    if (stream == nullptr) return STATUS_INVALID_DEVICE_REQUEST;

    ULONG inputSize = req->Irp ? IoGetCurrentIrpStackLocation(req->Irp)->Parameters.DeviceIoControl.InputBufferLength : 0;
    if (inputSize < sizeof(KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY)) return STATUS_INVALID_PARAMETER;

    PKSRTAUDIO_NOTIFICATION_EVENT_PROPERTY prop =
        (PKSRTAUDIO_NOTIFICATION_EVENT_PROPERTY)req->Irp->AssociatedIrp.SystemBuffer;
    if (prop->NotificationEvent == nullptr) return STATUS_INVALID_PARAMETER;

    if (registerEvent)
    {
        PKEVENT event = nullptr;
        NTSTATUS status = ObReferenceObjectByHandle(
            prop->NotificationEvent,
            EVENT_MODIFY_STATE,
            *ExEventObjectType,
            UserMode,
            (PVOID*)&event,
            nullptr);
        if (!NT_SUCCESS(status)) return status;
        return stream->AddNotificationEvent(event);
    }
    else
    {
        return stream->RemoveNotificationEvent(prop->NotificationEvent);
    }
}

// ============================== dispatcher ==================================

NTSTATUS CMiniportWaveRT::PropertyHandlerRtAudio(PPCPROPERTY_REQUEST req)
{
    PAGED_CODE();
    if (req == nullptr || req->PropertyItem == nullptr) return STATUS_INVALID_PARAMETER;

    ULONG id = req->PropertyItem->Id;

    // базовая поддержка — стандартная
    if (req->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        return PhonemicPropertyBasicSupport(req);
    }

    if (req->Verb & KSPROPERTY_TYPE_GET)
    {
        switch (id)
        {
        case KSPROPERTY_RTAUDIO_BUFFER:
        case KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION:
            return HandleRtAudioBuffer(req, id == KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION);
        case KSPROPERTY_RTAUDIO_HWLATENCY:
            return HandleRtAudioHwLatency(req);
        case KSPROPERTY_RTAUDIO_POSITIONREGISTER:
            return HandleRtAudioPositionRegister(req);
        case KSPROPERTY_RTAUDIO_CLOCKREGISTER:
            return HandleRtAudioClockRegister(req);
        default:
            return STATUS_NOT_SUPPORTED;
        }
    }

    if (req->Verb & KSPROPERTY_TYPE_SET)
    {
        switch (id)
        {
        case KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT:
            return HandleNotificationEvent(req, TRUE);
        case KSPROPERTY_RTAUDIO_UNREGISTER_NOTIFICATION_EVENT:
            return HandleNotificationEvent(req, FALSE);
        default:
            return STATUS_NOT_SUPPORTED;
        }
    }

    return STATUS_NOT_SUPPORTED;
}

// KSPROPERTY_PIN_PROPOSEDATAFORMAT: принимаем только наш фикс-формат
NTSTATUS CMiniportWaveRT::PropertyHandlerProposeDataFormat(PPCPROPERTY_REQUEST req)
{
    PAGED_CODE();
    if (req->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        return PhonemicPropertyBasicSupport(req);
    }
    if (req->Verb & KSPROPERTY_TYPE_SET)
    {
        // формат придёт в Irp->AssociatedIrp.SystemBuffer как KSDATAFORMAT_WAVEFORMATEX
        return STATUS_SUCCESS; // движок может выбрать — реальный формат фиксируется в NewStream
    }
    return STATUS_NOT_SUPPORTED;
}
