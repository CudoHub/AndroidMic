/*++
PhoneMic driver: реализация IMiniportWaveRT (описание, дескриптор фильтра,
свойства RTAudio, создание потока).
--*/
#include "common.h"
#include "adapter.h"
#include "minwavert.h"
#include "minwavertstream.h"

// фикс-формат: 48 кГц / 16 бит / mono (DataRangeIntersection)
static const KSDATAFORMAT_WAVEFORMATEX PhonemicDefaultFormat =
{
    {
        sizeof(KSDATAFORMAT_WAVEFORMATEX),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
    },
    {
        WAVE_FORMAT_PCM,
        1,                      // каналы
        PHONEMIC_SAMPLE_RATE,   // 48000
        PHONEMIC_SAMPLE_RATE * 2,
        2,
        16,
        0
    }
};

// =========================== automation: wave pin ===========================

static NTSTATUS PropertyHandlerWavePin(PPCPROPERTY_REQUEST PropertyRequest);

static PCPROPERTY_ITEM WavePinProperties[] =
{
    {
        &KSPROPSETID_RtAudio,
        KSPROPERTY_RTAUDIO_BUFFER,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET,
        PropertyHandlerWavePin
    },
    {
        &KSPROPSETID_RtAudio,
        KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION,
        KSPROPERTY_TYPE_GET,
        PropertyHandlerWavePin
    },
    {
        &KSPROPSETID_RtAudio,
        KSPROPERTY_RTAUDIO_HWLATENCY,
        KSPROPERTY_TYPE_GET,
        PropertyHandlerWavePin
    },
    {
        &KSPROPSETID_RtAudio,
        KSPROPERTY_RTAUDIO_POSITIONREGISTER,
        KSPROPERTY_TYPE_GET,
        PropertyHandlerWavePin
    },
    {
        &KSPROPSETID_RtAudio,
        KSPROPERTY_RTAUDIO_CLOCKREGISTER,
        KSPROPERTY_TYPE_GET,
        PropertyHandlerWavePin
    },
    {
        &KSPROPSETID_RtAudio,
        KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT,
        KSPROPERTY_TYPE_SET,
        PropertyHandlerWavePin
    },
    {
        &KSPROPSETID_RtAudio,
        KSPROPERTY_RTAUDIO_UNREGISTER_NOTIFICATION_EVENT,
        KSPROPERTY_TYPE_SET,
        PropertyHandlerWavePin
    },
    {
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_PROPOSEDATAFORMAT,
        KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        CMiniportWaveRT::PropertyHandlerProposeDataFormat
    },
};

static DEFINE_PCAUTOMATION_TABLE_PROP(WavePinAutomation, WavePinProperties);

// пин: capture (данные уходят клиенту), единственный инстанс
static PCPIN_DESCRIPTOR WavePinDescriptors[] =
{
    {
        0,                      // InstanceCount (заполняет порт)
        1,                      // PossibleInstanceCount
        &WavePinAutomation,     // AutomationTable
        {
            0, NULL,            // interfaces
            0, NULL,            // mediums
            KSPIN_DATAFLOW_OUT, // capture: данные наружу
            KSPIN_COMMUNICATION_SINK,
            STATICGUIDOF(KSCATEGORY_AUDIO),
            STATICGUIDOF(KSNODETYPE_MICROPHONE),
            0, NULL             // constrained data ranges
        }
    }
};

// свойства фильтра (пусто), узлов нет
static PCFILTER_DESCRIPTOR MiniportFilterWaveRt =
{
    0,                      // Version
    NULL,                   // AutomationTable (filter)
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(WavePinDescriptors),
    WavePinDescriptors,
    0, 0, NULL,             // nodes (нет)
    0, NULL,                // filter properties (нет)
    0, NULL,                // methods (нет)
    0, NULL,                // events (нет)
    0, NULL,                // connections (нет — один пин)
};

// =========================== CMiniportWaveRT ================================

CMiniportWaveRT::CMiniportWaveRT(_In_ PUNKNOWN OuterUnknown) :
    CUnknown("MiniportWaveRT", OuterUnknown)
{
    m_Format.wFormatTag = WAVE_FORMAT_PCM;
    m_Format.nChannels = 1;
    m_Format.nSamplesPerSec = PHONEMIC_SAMPLE_RATE;
    m_Format.nBlockAlign = 2;
    m_Format.wBitsPerSample = 16;
    m_Format.nAvgBytesPerSec = PHONEMIC_SAMPLE_RATE * 2;
}

CMiniportWaveRT::~CMiniportWaveRT()
{
    if (m_ServiceGroup)
    {
        m_Port->ReleaseServiceGroup(m_ServiceGroup);
        m_ServiceGroup = nullptr;
    }
    if (m_Port) m_Port->Release();
}

NTSTATUS CMiniportWaveRT::Create(
    _Outptr_ PUNKNOWN* Unknown,
    _In_ REFCLSID Clsid,
    _In_ POOL_TYPE PoolType,
    _In_ PUNKNOWN OuterUnknown)
{
    UNREFERENCED_PARAMETER(Clsid);
    UNREFERENCED_PARAMETER(PoolType);

    CMiniportWaveRT* obj = new (NonPagedPoolNx, PHONEMIC_TAG_GEN) CMiniportWaveRT(OuterUnknown);
    if (obj == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    *Unknown = (PUNKNOWN)(IMiniportWaveRT*)obj;
    obj->AddRef();
    return STATUS_SUCCESS;
}

IMP_IMiniportWaveRT::GetDeviceDescription(
    _Out_ PDEVICE_DESCRIPTION* DeviceDescription
    )
{
    if (m_DeviceDescription == nullptr)
    {
        m_DeviceDescription = (PDEVICE_DESCRIPTION)ExAllocatePool2(
            POOL_FLAG_NON_PAGED, sizeof(DEVICE_DESCRIPTION), PHONEMIC_TAG_GEN);
        if (m_DeviceDescription == nullptr) return STATUS_INSUFFICIENT_RESOURCES;
        RtlZeroMemory(m_DeviceDescription, sizeof(DEVICE_DESCRIPTION));
        m_DeviceDescription->Master = TRUE;
        m_DeviceDescription->ScatterGather = TRUE;
        m_DeviceDescription->Dma32BitAddresses = TRUE;
        m_DeviceDescription->Dma64BitAddresses = TRUE;
        m_DeviceDescription->BusNumber = 0;
        m_DeviceDescription->MaximumLength = PHONEMIC_MAX_WAVERT_BUFFER;
    }
    *DeviceDescription = m_DeviceDescription;
    return STATUS_SUCCESS;
}

IMP_IMiniportWaveRT::DataRangeIntersection(
    _In_ ULONG PinId,
    _In_ PKSDATARANGE ClientDataRange,
    _In_ PKSDATARANGE MiniportDataRange,
    _In_ ULONG OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatSize) PVOID ResultantFormat,
    _Out_ PULONG ResultantFormatSize
    )
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(ClientDataRange);
    UNREFERENCED_PARAMETER(MiniportDataRange);

    // фикс-формат: 48k/16/mono (движок сам ресемплит)
    ULONG size = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    *ResultantFormatSize = size;
    if (OutputBufferLength < size) return STATUS_BUFFER_OVERFLOW;
    if (ResultantFormat)
    {
        RtlCopyMemory(ResultantFormat, &PhonemicDefaultFormat, size);
    }
    return STATUS_SUCCESS;
}

IMP_IMiniportWaveRT::GetDescription(
    _Out_ PCFILTER_DESCRIPTOR** FilterDescriptor
    )
{
    *FilterDescriptor = &MiniportFilterWaveRt;
    return STATUS_SUCCESS;
}

IMP_IMiniportWaveRT::Init(
    _In_ PUNKNOWN UnknownAdapter,
    _In_ PRESOURCELIST ResourceList,
    _In_ PPORTWAVERT Port,
    _Out_ PSERVICEGROUP* ServiceGroup
    )
{
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    PAGED_CODE();

    ASSERT(Port != nullptr);
    m_Port = Port;
    m_Port->AddRef();

    NTSTATUS ntStatus = PcNewServiceGroup(&m_ServiceGroup, PHONEMIC_TAG_GEN);
    if (NT_SUCCESS(ntStatus))
    {
        m_Port->RegisterServiceGroup(m_ServiceGroup);
    }
    *ServiceGroup = m_ServiceGroup;
    if (m_ServiceGroup) m_ServiceGroup->AddRef();

    return STATUS_SUCCESS;
}

IMP_IMiniportWaveRT::NewStream(
    _Out_ PIMiniportWaveRTStream* Stream,
    _In_ PPORTWAVERTSTREAM PortStream,
    _In_ ULONG Pin,
    _In_ BOOLEAN Capture,
    _In_ PKSDATAFORMAT DataFormat
    )
{
    UNREFERENCED_PARAMETER(Pin);
    PAGED_CODE();

    if (!Capture)
    {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "PhoneMic: render stream not supported\n");
        return STATUS_INVALID_PARAMETER;
    }

    // применяем формат (если движок предложил валидный)
    PWAVEFORMATEX wf = &((PKSDATAFORMAT_WAVEFORMATEX)DataFormat)->WaveFormatEx;
    if (wf != nullptr &&
        DataFormat->FormatSize >= sizeof(KSDATAFORMAT_WAVEFORMATEX) &&
        wf->wFormatTag == WAVE_FORMAT_PCM &&
        wf->nChannels == 1 &&
        wf->wBitsPerSample == 16 &&
        (wf->nSamplesPerSec == 48000 || wf->nSamplesPerSec == 44100))
    {
        m_Format = *wf;
    }

    // старый поток (если был) — ссылка принадлежит порту, просто забываем указатель
    m_Stream = nullptr;

    CMiniportWaveRTStream* stream = nullptr;
    NTSTATUS ntStatus = CMiniportWaveRTStream::Create(
        (PUNKNOWN*)&stream,
        GUID_NULL, NonPagedPoolNx, nullptr, this, PortStream, Capture);
    if (!NT_SUCCESS(ntStatus)) return ntStatus;

    ntStatus = stream->Init(this, PortStream, Capture);
    if (!NT_SUCCESS(ntStatus))
    {
        stream->Release();
        return ntStatus;
    }

    m_Stream = stream;
    *Stream = (PIMiniportWaveRTStream)stream;   // передача ссылки
    return STATUS_SUCCESS;
}

IMP_IMiniportWaveRT::Service(VOID)
{
    return STATUS_SUCCESS;
}

// =========================== фабрика минипорта ==============================

NTSTATUS PhonemicCreateMiniportWaveRT(
    _Outptr_ PUNKNOWN* Unknown,
    _In_ REFCLSID Clsid,
    _In_ POOL_TYPE PoolType,
    _In_ PUNKNOWN OuterUnknown)
{
    return CMiniportWaveRT::Create(Unknown, Clsid, PoolType, OuterUnknown);
}
