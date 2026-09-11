/*++
PhoneMic driver: топологический минипорт (IMiniportTopology).
Граф: [MIC bridge pin0] -> [VOLUME] -> [MUTE] -> [wave pin1].
--*/
#pragma once
#include "common.h"

class CMiniportTopology :
    public IMiniportTopology,
    public CUnknown
{
public:
    DECLARE_USING_UNKNOWN()

    CMiniportTopology(_In_ PUNKNOWN OuterUnknown);
    ~CMiniportTopology();

    // IMiniport
    IMP_IMiniport(GetDeviceDescription);
    // IMiniportTopology
    IMP_IMiniportTopology(DataRangeIntersection);
    IMP_IMiniportTopology(GetDescription);
    IMP_IMiniportTopology(Init);

    // фабрика
    static NTSTATUS Create(
        _Outptr_ PUNKNOWN* Unknown,
        _In_ REFCLSID Clsid,
        _In_ POOL_TYPE PoolType,
        _In_ PUNKNOWN OuterUnknown);

    // обработчики свойств узлов (volume/mute)
    static NTSTATUS PropertyHandlerTopo(PPCPROPERTY_REQUEST PropertyRequest);

protected:
    PDEVICE_DESCRIPTION m_DeviceDescription = nullptr;
};
