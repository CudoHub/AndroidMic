/*++
PhoneMic driver: реализация топологии (VOLUME / MUTE узлы, jack description).
--*/
#include "common.h"
#include "adapter.h"
#include "mintopo.h"

// узлы
#define TOPO_NODE_VOLUME  0
#define TOPO_NODE_MUTE    1

// сохранённые значения (не влияют на данные — громкость применяет движок)
static LONG g_TopologyVolume = 0x0000C000;  // 0 dB (фикс-точка, KSDATAFORMAT... уровень)
static BOOLEAN g_TopologyMute = FALSE;

//=============================================================================
// automation: пины
//=============================================================================

static PCPROPERTY_ITEM TopoPinProperties[] =
{
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION,
        KSPROPERTY_TYPE_GET,
        CMiniportTopology::PropertyHandlerTopo
    },
};
DEFINE_PCAUTOMATION_TABLE_PROP(TopoPinAutomation, TopoPinProperties);

static PCPIN_DESCRIPTOR TopoPinDescriptors[] =
{
    // pin 0: мост «микрофон» — источник данных в граф
    {
        0, 1, &TopoPinAutomation,
        {
            0, NULL,
            0, NULL,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_BRIDGE,
            STATICGUIDOF(KSCATEGORY_AUDIO),
            STATICGUIDOF(KSNODETYPE_MICROPHONE),
            0, NULL
        }
    },
    // pin 1: к wave-фильтру
    {
        0, 1, &TopoPinAutomation,
        {
            0, NULL,
            0, NULL,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            STATICGUIDOF(KSCATEGORY_AUDIO),
            STATICGUIDOF(KSNODETYPE_MICROPHONE),
            0, NULL
        }
    }
};

//=============================================================================
// automation: узлы
//=============================================================================

static PCPROPERTY_ITEM VolumeNodeProperties[] =
{
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_VOLUMELEVEL,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        CMiniportTopology::PropertyHandlerTopo
    },
};
DEFINE_PCAUTOMATION_TABLE_PROP(VolumeNodeAutomation, VolumeNodeProperties);

static PCPROPERTY_ITEM MuteNodeProperties[] =
{
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_MUTE,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        CMiniportTopology::PropertyHandlerTopo
    },
};
DEFINE_PCAUTOMATION_TABLE_PROP(MuteNodeAutomation, MuteNodeProperties);

static PCNODE_DESCRIPTOR TopoNodeDescriptors[] =
{
    // node 0: VOLUME
    {
        0,                          // Flags
        &VolumeNodeAutomation,      // AutomationTable
        &KSNODETYPE_VOLUME,         // Type
        nullptr                     // Name (по типу)
    },
    // node 1: MUTE
    {
        0,
        &MuteNodeAutomation,
        &KSNODETYPE_MUTE,
        nullptr
    }
};

// внутренние соединения графа: pin0 -> volume -> mute -> pin1
static PCCONNECTION_DESCRIPTOR TopoConnections[] =
{
    { PCFILTER_NODE,    PIN_TOPO_MIC,    TOPO_NODE_VOLUME, 1 },
    { TOPO_NODE_VOLUME, 1,               TOPO_NODE_MUTE,   1 },
    { TOPO_NODE_MUTE,   1,               PCFILTER_NODE,    PIN_TOPO_WAVESINK },
};

static PCFILTER_DESCRIPTOR MiniportFilterTopology =
{
    0,                          // Version
    NULL,                       // filter automation
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(TopoPinDescriptors),
    TopoPinDescriptors,
    sizeof(PCNODE_DESCRIPTOR),
    SIZEOF_ARRAY(TopoNodeDescriptors),
    TopoNodeDescriptors,
    0, NULL,                    // filter properties (нет)
    0, NULL,                    // methods (нет)
    0, NULL,                    // events (нет)
    SIZEOF_ARRAY(TopoConnections),  // ConnectionCount
    TopoConnections,                // ConnectionList
};

//=============================================================================

CMiniportTopology::CMiniportTopology(_In_ PUNKNOWN OuterUnknown) :
    CUnknown("MiniportTopology", OuterUnknown)
{
}

CMiniportTopology::~CMiniportTopology()
{
}

NTSTATUS CMiniportTopology::Create(
    _Outptr_ PUNKNOWN* Unknown,
    _In_ REFCLSID Clsid,
    _In_ POOL_TYPE PoolType,
    _In_ PUNKNOWN OuterUnknown)
{
    UNREFERENCED_PARAMETER(Clsid);
    UNREFERENCED_PARAMETER(PoolType);

    CMiniportTopology* obj = new (NonPagedPoolNx, PHONEMIC_TAG_GEN) CMiniportTopology(OuterUnknown);
    if (obj == nullptr) return STATUS_INSUFFICIENT_RESOURCES;
    *Unknown = (PUNKNOWN)(IMiniportTopology*)obj;
    obj->AddRef();
    return STATUS_SUCCESS;
}

IMP_IMiniport::GetDeviceDescription(_Out_ PDEVICE_DESCRIPTION* DeviceDescription)
{
    if (m_DeviceDescription == nullptr)
    {
        m_DeviceDescription = (PDEVICE_DESCRIPTION)ExAllocatePool2(
            POOL_FLAG_NON_PAGED, sizeof(DEVICE_DESCRIPTION), PHONEMIC_TAG_GEN);
        if (m_DeviceDescription == nullptr) return STATUS_INSUFFICIENT_RESOURCES;
        RtlZeroMemory(m_DeviceDescription, sizeof(DEVICE_DESCRIPTION));
        m_DeviceDescription->Master = TRUE;
    }
    *DeviceDescription = m_DeviceDescription;
    return STATUS_SUCCESS;
}

IMP_IMiniportTopology::DataRangeIntersection(
    _In_ ULONG PinId,
    _In_ PKSDATARANGE ClientDataRange,
    _In_ PKSDATARANGE MiniportDataRange,
    _In_ ULONG OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatSize) PVOID ResultantFormat,
    _Out_ PULONG ResultantFormatSize)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(ClientDataRange);
    UNREFERENCED_PARAMETER(MiniportDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    *ResultantFormatSize = 0;
    return STATUS_NOT_SUPPORTED;
}

IMP_IMiniportTopology::GetDescription(_Out_ PCFILTER_DESCRIPTOR** FilterDescriptor)
{
    *FilterDescriptor = &MiniportFilterTopology;
    return STATUS_SUCCESS;
}

IMP_IMiniportTopology::Init(
    _In_ PUNKNOWN UnknownAdapter,
    _In_ PRESOURCELIST ResourceList,
    _In_ PPORTTOPOLOGY Port)
{
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(Port);
    PAGED_CODE();
    return STATUS_SUCCESS;
}

// =============================== свойства ===================================

NTSTATUS CMiniportTopology::PropertyHandlerTopo(PPCPROPERTY_REQUEST req)
{
    PAGED_CODE();
    if (req == nullptr || req->Property == nullptr) return STATUS_INVALID_PARAMETER;

    if (req->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        return PcPropertyHandlerBasicSupport(req);
    }

    // jack description
    if (req->Property->Set == KSPROPSETID_Jack &&
        req->Property->Id == KSPROPERTY_JACK_DESCRIPTION)
    {
        if (!(req->Verb & KSPROPERTY_TYPE_GET)) return STATUS_NOT_SUPPORTED;
        KSJACK_DESCRIPTION jack;
        jack.ChannelMapping = 1;             // KSAUDIO_SPEAKER_MONO
        jack.Color = 0;
        jack.ConnectionType = eConnType3Point5mm;
        jack.GeoLocation = eGeoLocRear;
        jack.GenLocation = eGenLocPrimaryBox;
        jack.PortConnection = ePortConnJack;
        jack.IsConnected = TRUE;

        if (req->ValueSize == 0) { req->ValueSize = sizeof(jack); return STATUS_SUCCESS; }
        if (req->ValueSize < sizeof(jack)) return STATUS_BUFFER_TOO_SMALL;
        *(PKSJACK_DESCRIPTION)req->Value = jack;
        req->ValueSize = sizeof(jack);
        return STATUS_SUCCESS;
    }

    // volume level (per channel, mono)
    if (req->Property->Set == KSPROPSETID_Audio &&
        req->Property->Id == KSPROPERTY_AUDIO_VOLUMELEVEL)
    {
        if (req->Verb & KSPROPERTY_TYPE_GET)
        {
            if (req->ValueSize < sizeof(LONG)) return STATUS_BUFFER_TOO_SMALL;
            *(PLONG)req->Value = g_TopologyVolume;
            req->ValueSize = sizeof(LONG);
            return STATUS_SUCCESS;
        }
        if (req->Verb & KSPROPERTY_TYPE_SET)
        {
            if (req->ValueSize < sizeof(LONG)) return STATUS_BUFFER_TOO_SMALL;
            g_TopologyVolume = *(PLONG)req->Value;
            return STATUS_SUCCESS;
        }
    }

    // mute
    if (req->Property->Set == KSPROPSETID_Audio &&
        req->Property->Id == KSPROPERTY_AUDIO_MUTE)
    {
        if (req->Verb & KSPROPERTY_TYPE_GET)
        {
            if (req->ValueSize < sizeof(BOOLEAN)) return STATUS_BUFFER_TOO_SMALL;
            *(PBOOLEAN)req->Value = g_TopologyMute;
            req->ValueSize = sizeof(BOOLEAN);
            return STATUS_SUCCESS;
        }
        if (req->Verb & KSPROPERTY_TYPE_SET)
        {
            if (req->ValueSize < sizeof(BOOLEAN)) return STATUS_BUFFER_TOO_SMALL;
            g_TopologyMute = *(PBOOLEAN)req->Value;
            return STATUS_SUCCESS;
        }
    }

    return STATUS_NOT_SUPPORTED;
}

// фабрика минипорта топологии
NTSTATUS PhonemicCreateMiniportTopology(
    _Outptr_ PUNKNOWN* Unknown,
    _In_ REFCLSID Clsid,
    _In_ POOL_TYPE PoolType,
    _In_ PUNKNOWN OuterUnknown)
{
    return CMiniportTopology::Create(Unknown, Clsid, PoolType, OuterUnknown);
}
