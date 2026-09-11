/*++
PhoneMic driver: объявления адаптера.
--*/
#pragma once
#include "common.h"

// Идентификаторы пинов (порядок = порядок в PCFILTER_DESCRIPTOR).
#define PIN_WAVE_CAPTURE        0   // wave-фильтр: единственный capture-пин
#define PIN_TOPO_MIC            0   // топология: мост (микрофон, источник данных)
#define PIN_TOPO_WAVESINK       1   // топология: к wave-фильтру (приёмник graph-данных)

// тип фабрики минипорта (совместим с PcNewMiniport)
typedef NTSTATUS (*PFNCREATEMINIPORT)(
    _Outptr_ PUNKNOWN* Unknown,
    _In_ REFCLSID Clsid,
    _In_ POOL_TYPE PoolType,
    _In_ PUNKNOWN OuterUnknown);

// start device (PCPFNSTARTDEVICE)
NTSTATUS PhonemicStartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList);

// фабрики минипортов (создают CUnknown-объекты)
NTSTATUS PhonemicCreateMiniportWaveRT(
    _Outptr_ PUNKNOWN* Unknown,
    _In_ REFCLSID,
    _In_ POOL_TYPE PoolType,
    _In_ PUNKNOWN OuterUnknown);

NTSTATUS PhonemicCreateMiniportTopology(
    _Outptr_ PUNKNOWN* Unknown,
    _In_ REFCLSID,
    _In_ POOL_TYPE PoolType,
    _In_ PUNKNOWN OuterUnknown);

DRIVER_UNLOAD PhonemicUnload;
