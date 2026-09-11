/*++
PhoneMic driver: реализация топологии (VOLUME / MUTE узлы, jack description).
Сигнатуры и структуры — в точности по portcls.h 26100.
--*/
#include "common.h"
#include "adapter.h"
#include "mintopo.h"

// узлы
#define TOPO_NODE_VOLUME  0
#define TOPO_NODE_MUTE    1

// сохранённые значения (не влияют на данные — громкость применяет движок)
static LONG g_TopologyVolume = 0x0000C000;  // 0 dB (фикс-точка)
static BOOLEAN g_TopologyMute = FALSE;

//=============================================================================
// стандартный BASICSUPPORT: KSPROPERTY_DESCRIPTION с флагами доступа
//=============================================================================
NTSTATUS PhonemicPropertyBasicSupport(_In_ PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();
    ASSERT(PropertyRequest);
    if (!PropertyRequest) return STATUS_INVALID_PARAMETER;

    ULONG access = KSPROPERTY_TYPE_BASICSUPPORT |
                   (PropertyRequest->PropertyItem->Flags &
                    (KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET));

    if (PropertyRequest->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION))
    {
        PKSPROPERTY_DESCRIPTION desc = (PKSPROPERTY_DESCRIPTION)PropertyRequest->Value;
        RtlZeroMemory(desc, sizeof(KSPROPERTY_DESCRIPTION));
        desc->AccessFlags = access;
        desc->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION);
        PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        return STATUS_SUCCESS;
    }
    if (PropertyRequest->ValueSize == 0)
    {
        PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        return STATUS_SUCCESS;   // запрос размера
    }
    PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
    return STATUS_BUFFER_TOO_SMALL;
}

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

// PCPIN_DESCRIPTOR: {MaxGlobal, MaxFilter, MinFilter, Automation, KSPIN_DESCRIPTOR}
static PCPIN_DESCRIPTOR TopoPinDescriptors[] =
{
    // pin 0: мост «микрофон» — источник данных в граф
    {
        1, 1, 0,
        &TopoPinAutomation,
        {
            0, NULL,            // interfaces
            0, NULL,            // mediums
            0, NULL,            // data ranges (мост — без ограничений)
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_BRIDGE,
            &KSCATEGORY_AUDIO,
            &KSNODETYPE_MICROPHONE,
            0                   // Reserved
        }
    },
    // pin 1: к wave-фильтру
    {
        1, 1, 0,
        &TopoPinAutomation,
        {
            0, NULL,
            0, NULL,
            0, NULL,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            &KSNODETYPE_MICROPHONE,
            0
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

// PCFILTER_DESCRIPTOR: {Version, Automation, PinSize, PinCount, Pins,
//                       NodeSize, NodeCount, Nodes,
//                       ConnectionCount, Connections, CategoryCount, Categories}
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
    SIZEOF_ARRAY(TopoConnections),  // ConnectionCount
    TopoConnections,                // Connections
    0, NULL                         // CategoryCount, Categories
};

//=============================================================================

CMiniportTopology::CMiniportTopology(_In_ PUNKNOWN OuterUnknown) :
    CUnknown(OuterUnknown)
{
}

CMiniportTopology::~CMiniportTopology()
{
}

STDMETHODIMP_(NTSTATUS)
CMiniportTopology::NonDelegatingQueryInterface(_In_ REFIID Interface, _COM_Outptr_ PVOID* Object)
{
    ASSERT(Object);
    if (!Object) return STATUS_INVALID_PARAMETER;

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = (PVOID)(PUNKNOWN)(IMiniportTopology*)this;
        ((PUNKNOWN)*Object)->AddRef();
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportTopology))
    {
        *Object = (PVOID)(IMiniportTopology*)this;
        ((PUNKNOWN)(IMiniportTopology*)this)->AddRef();
    }
    else
    {
        *Object = nullptr;
        return CUnknown::NonDelegatingQueryInterface(Interface, Object);
    }
    return STATUS_SUCCESS;
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

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportTopology::DataRangeIntersection(
    _In_ ULONG PinId,
    _In_ PKSDATARANGE ClientDataRange,
    _In_ PKSDATARANGE MiniportDataRange,
    _In_ ULONG OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength) PVOID ResultantFormat,
    _Out_ PULONG ResultantFormatLength)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(ClientDataRange);
    UNREFERENCED_PARAMETER(MiniportDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    PAGED_CODE();
    *ResultantFormatLength = 0;
    return STATUS_NOT_SUPPORTED;
}

STDMETHODIMP_(NTSTATUS)
CMiniportTopology::GetDescription(_Out_ PPCFILTER_DESCRIPTOR* FilterDescriptor)
{
    PAGED_CODE();
    ASSERT(FilterDescriptor);
    if (!FilterDescriptor) return STATUS_INVALID_PARAMETER;
    *FilterDescriptor = &MiniportFilterTopology;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportTopology::Init(
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
#pragma code_seg()

// =============================== свойства ===================================

NTSTATUS CMiniportTopology::PropertyHandlerTopo(PPCPROPERTY_REQUEST req)
{
    PAGED_CODE();
    if (req == nullptr || req->PropertyItem == nullptr) return STATUS_INVALID_PARAMETER;

    if (req->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        return PhonemicPropertyBasicSupport(req);
    }

    // jack description
    if (req->PropertyItem->Set == KSPROPSETID_Jack &&
        req->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION)
    {
        if (!(req->Verb & KSPROPERTY_TYPE_GET)) return STATUS_NOT_SUPPORTED;
        KSJACK_DESCRIPTION jack;
        jack.ChannelMapping = KSAUDIO_SPEAKER_MONO;
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
    if (req->PropertyItem->Set == KSPROPSETID_Audio &&
        req->PropertyItem->Id == KSPROPERTY_AUDIO_VOLUMELEVEL)
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
    if (req->PropertyItem->Set == KSPROPSETID_Audio &&
        req->PropertyItem->Id == KSPROPERTY_AUDIO_MUTE)
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
