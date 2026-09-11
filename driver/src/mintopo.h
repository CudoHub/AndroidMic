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
    DECLARE_STD_UNKNOWN()

    CMiniportTopology(_In_ PUNKNOWN OuterUnknown);
    ~CMiniportTopology();

    // IMiniportTopology = IMiniport (GetDescription, DataRangeIntersection) + Init
    IMP_IMiniportTopology;   // макрос НЕ завершается ';' — точка с запятой обязательна

    // фабрика
    static NTSTATUS Create(
        _Outptr_ PUNKNOWN* Unknown,
        _In_ REFCLSID Clsid,
        _In_ POOL_TYPE PoolType,
        _In_ PUNKNOWN OuterUnknown);

    // обработчики свойств узлов (volume/mute)
    static NTSTATUS PropertyHandlerTopo(PPCPROPERTY_REQUEST PropertyRequest);
};
