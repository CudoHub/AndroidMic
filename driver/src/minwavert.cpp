/*++
PhoneMic driver: реализация IMiniportWaveRT (описание, дескриптор фильтра,
создание потока). Сигнатуры методов — в точности по portcls.h 26100.
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
// PCPIN_DESCRIPTOR: {MaxGlobalInstanceCount, MaxFilterInstanceCount,
//                    MinFilterInstanceCount, AutomationTable, KSPIN_DESCRIPTOR}
static PCPIN_DESCRIPTOR WavePinDescriptors[] =
{
    {
        1, 1, 0,                // max global / max filter / min filter
        &WavePinAutomation,
        {
            0, NULL,            // interfaces
            0, NULL,            // mediums
            PHONEMIC_PIN_DATA_RANGE_COUNT, PhonemicPinDataRanges,
            KSPIN_DATAFLOW_OUT, // capture: данные наружу
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            &KSNODETYPE_MICROPHONE,
            0                   // Reserved
        }
    }
};

// свойства фильтра (пусто), узлов нет
// PCFILTER_DESCRIPTOR: {Version, AutomationTable, PinSize, PinCount, Pins,
//                       NodeSize, NodeCount, Nodes,
//                       ConnectionCount, Connections, CategoryCount, Categories}
static PCFILTER_DESCRIPTOR MiniportFilterWaveRt =
{
    0,                      // Version
    NULL,                   // AutomationTable (filter)
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(WavePinDescriptors),
    WavePinDescriptors,
    0, 0, NULL,             // nodes (нет)
    0, NULL,                // connections (нет — один пин)
    0, NULL                 // categories (нет)
};

// =========================== CMiniportWaveRT ================================

CMiniportWaveRT::CMiniportWaveRT(_In_ PUNKNOWN OuterUnknown) :
    CUnknown(OuterUnknown)
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

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::NonDelegatingQueryInterface(_In_ REFIID Interface, _COM_Outptr_ PVOID* Object)
{
    ASSERT(Object);
    if (!Object) return STATUS_INVALID_PARAMETER;

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = (PVOID)(PUNKNOWN)(IMiniportWaveRT*)this;
        ((PUNKNOWN)*Object)->AddRef();
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRT))
    {
        *Object = (PVOID)(IMiniportWaveRT*)this;
        ((PUNKNOWN)(IMiniportWaveRT*)this)->AddRef();
    }
    else
    {
        *Object = nullptr;
        return CUnknown::NonDelegatingQueryInterface(Interface, Object);
    }
    return STATUS_SUCCESS;
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

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::GetDeviceDescription(_Out_ PDEVICE_DESCRIPTION DeviceDescription)
{
    PAGED_CODE();
    ASSERT(DeviceDescription);
    if (!DeviceDescription) return STATUS_INVALID_PARAMETER;

    if (m_DeviceDescription.Master == FALSE)
    {
        RtlZeroMemory(&m_DeviceDescription, sizeof(DEVICE_DESCRIPTION));
        m_DeviceDescription.Master = TRUE;
        m_DeviceDescription.ScatterGather = TRUE;
        m_DeviceDescription.Dma32BitAddresses = TRUE;
        m_DeviceDescription.Dma64BitAddresses = TRUE;
        m_DeviceDescription.BusNumber = 0;
        m_DeviceDescription.MaximumLength = PHONEMIC_MAX_WAVERT_BUFFER;
    }
    RtlCopyMemory(DeviceDescription, &m_DeviceDescription, sizeof(DEVICE_DESCRIPTION));
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::DataRangeIntersection(
    _In_ ULONG PinId,
    _In_ PKSDATARANGE ClientDataRange,
    _In_ PKSDATARANGE MiniportDataRange,
    _In_ ULONG OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength) PVOID ResultantFormat,
    _Out_ PULONG ResultantFormatLength
    )
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(ClientDataRange);
    UNREFERENCED_PARAMETER(MiniportDataRange);
    PAGED_CODE();

    // фикс-формат: 48k/16/mono (движок сам ресемплит)
    ULONG size = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    *ResultantFormatLength = size;
    if (OutputBufferLength < size) return STATUS_BUFFER_OVERFLOW;
    if (ResultantFormat)
    {
        RtlCopyMemory(ResultantFormat, &PhonemicDefaultFormat, size);
    }
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::GetDescription(_Out_ PPCFILTER_DESCRIPTOR* FilterDescriptor)
{
    PAGED_CODE();
    ASSERT(FilterDescriptor);
    if (!FilterDescriptor) return STATUS_INVALID_PARAMETER;
    *FilterDescriptor = &MiniportFilterWaveRt;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::Init(
    _In_ PUNKNOWN UnknownAdapter,
    _In_ PRESOURCELIST ResourceList,
    _In_ PPORTWAVERT Port
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
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::NewStream(
    _Out_ PMINIPORTWAVERTSTREAM* Stream,
    _In_ PPORTWAVERTSTREAM PortStream,
    _In_ ULONG Pin,
    _In_ BOOLEAN Capture,
    _In_ PKSDATAFORMAT DataFormat
    )
{
    UNREFERENCED_PARAMETER(Pin);
    PAGED_CODE();

    if (!Stream || !PortStream) return STATUS_INVALID_PARAMETER;

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
        wf->nSamplesPerSec == PHONEMIC_SAMPLE_RATE)
    {
        m_Format = *wf;
    }

    // старый поток (если был) — ссылка принадлежит порту, просто забываем указатель
    m_Stream = nullptr;

    CMiniportWaveRTStream* stream = nullptr;
    NTSTATUS ntStatus = CMiniportWaveRTStream::Create(
        &stream, this, PortStream, Capture, nullptr);
    if (!NT_SUCCESS(ntStatus)) return ntStatus;

    ntStatus = stream->Init(this, PortStream, Capture);
    if (!NT_SUCCESS(ntStatus))
    {
        stream->Release();
        return ntStatus;
    }

    m_Stream = stream;
    *Stream = (PMINIPORTWAVERTSTREAM)stream;   // передача ссылки
    return STATUS_SUCCESS;
}
#pragma code_seg()

// =========================== фабрика минипорта ==============================

NTSTATUS PhonemicCreateMiniportWaveRT(
    _Outptr_ PUNKNOWN* Unknown,
    _In_ REFCLSID Clsid,
    _In_ POOL_TYPE PoolType,
    _In_ PUNKNOWN OuterUnknown)
{
    return CMiniportWaveRT::Create(Unknown, Clsid, PoolType, OuterUnknown);
}
