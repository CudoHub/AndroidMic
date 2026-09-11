/*++
PhoneMic driver: WaveRT capture miniport (IMiniportWaveRT).
Соответствует portcls.h 26100: IMiniportWaveRT = IMiniport(GetDescription,
DataRangeIntersection) + Init + NewStream + GetDeviceDescription.
--*/
#pragma once
#include "common.h"

class CMiniportWaveRTStream;

//=============================================================================
// Диапазоны данных capture-пина (48 кГц, 16 бит, mono — движок сам ресемплит)
//=============================================================================
static const KSDATARANGE_AUDIO PhonemicPinDataRangesAudio[] =
{
    {
        {
            sizeof(KSDATARANGE_AUDIO),
            0,                       // Flags
            0,                       // SampleSize
            0,                       // Reserved
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        1,                           // MaximumChannels (mono)
        16,                          // MinimumBitsPerSample
        16,                          // MaximumBitsPerSample
        48000,                       // MinimumSampleFrequency
        48000                        // MaximumSampleFrequency (фикс — 48 кГц)
    },
};

static const PKSDATARANGE PhonemicPinDataRanges[] =
{
    (PKSDATARANGE)&PhonemicPinDataRangesAudio[0],
};

#define PHONEMIC_PIN_DATA_RANGE_COUNT  (SIZEOF_ARRAY(PhonemicPinDataRanges))

//=============================================================================
class CMiniportWaveRT :
    public IMiniportWaveRT,
    public CUnknown
{
public:
    DECLARE_STD_UNKNOWN()

    CMiniportWaveRT(_In_ PUNKNOWN OuterUnknown);
    ~CMiniportWaveRT();

    // IMiniport + IMiniportWaveRT: GetDescription, DataRangeIntersection,
    // Init, NewStream, GetDeviceDescription
    IMP_IMiniportWaveRT

    // фабрика
    static NTSTATUS Create(
        _Outptr_ PUNKNOWN* Unknown,
        _In_ REFCLSID Clsid,
        _In_ POOL_TYPE PoolType,
        _In_ PUNKNOWN OuterUnknown);

    // обработчики свойств (automation table)
    static NTSTATUS PropertyHandlerRtAudio(PPCPROPERTY_REQUEST PropertyRequest);
    static NTSTATUS PropertyHandlerProposeDataFormat(PPCPROPERTY_REQUEST PropertyRequest);

    PPORTWAVERT GetPort() { return m_Port; }
    CMiniportWaveRTStream* GetStream() { return m_Stream; }
    VOID SetStream(CMiniportWaveRTStream* stream) { m_Stream = stream; }
    PWAVEFORMATEX GetFormat() { return &m_Format; }

protected:
    PPORTWAVERT m_Port = nullptr;
    PSERVICEGROUP m_ServiceGroup = nullptr;
    CMiniportWaveRTStream* m_Stream = nullptr;
    DEVICE_DESCRIPTION m_DeviceDescription = {};
    WAVEFORMATEX m_Format = {};
};

typedef CMiniportWaveRT* PMINIPORTWAVERT;

#include "minwavertstream.h"
