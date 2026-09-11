/*++
PhoneMic driver: WaveRT capture miniport (IMiniportWaveRT).
--*/
#pragma once
#include "common.h"

class CMiniportWaveRTStream;

//=============================================================================
// Диапазоны данных capture-пина (48к/44.1к, 16 бит, mono)
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
        44100,                       // MinimumSampleFrequency
        48000                        // MaximumSampleFrequency
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
    DECLARE_USING_UNKNOWN()

    CMiniportWaveRT(_In_ PUNKNOWN OuterUnknown);
    ~CMiniportWaveRT();

    // IMiniport
    IMP_IMiniport(GetDeviceDescription);
    // IMiniportWaveRT
    IMP_IMiniportWaveRT(DataRangeIntersection);
    IMP_IMiniportWaveRT(GetDescription);
    IMP_IMiniportWaveRT(Init);
    IMP_IMiniportWaveRT(NewStream);
    IMP_IMiniportWaveRT(Service);

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
    PDEVICE_DESCRIPTION m_DeviceDescription = nullptr;
    PPORTWAVERT m_Port = nullptr;
    PSERVICEGROUP m_ServiceGroup = nullptr;
    CMiniportWaveRTStream* m_Stream = nullptr;
    WAVEFORMATEX m_Format = {};
};

typedef CMiniportWaveRT* PMINIPORTWAVERT;

#include "minwavertstream.h"
