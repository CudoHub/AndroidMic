/*++
PhoneMic driver: общие определения.
--*/
#pragma once

// ВАЖНО: initguid.h сюда включать НЕЛЬЗЯ — он раскрывает все DEFINE_GUID в
// определения, и каждый .obj получит свои копии GUID (LNK2005). Единственное
// ТУ с определениями — src/guids.cpp.
#include <ntddk.h>
#include <portcls.h>
#include <stdunk.h>     // CUnknown, DECLARE_STD_UNKNOWN, операторы new/delete
#include <ks.h>
#include <ksmedia.h>
#include <wdm.h>
#include "phonemic_ioctl.h"

// ============================ pool tags ====================================
#define PHONEMIC_TAG_RING    'gmrP'  // Prmg
#define PHONEMIC_TAG_REG     'gerP'  // Preg
#define PHONEMIC_TAG_GEN     'negP'  // Pgen
#define PHONEMIC_TAG_CTL     'ltcP'  // Pctl

#ifndef SIZEOF_ARRAY
#define SIZEOF_ARRAY(a)  (sizeof(a) / sizeof((a)[0]))
#endif

// ============================ параметры ====================================
#define PHONEMIC_SAMPLE_RATE         48000
#define PHONEMIC_PERIOD_MICROSEC     5000              // 5 мс DPC
#define PHONEMIC_RING_SAMPLES        4800              // 100 мс
#define PHONEMIC_RING_BYTES          (PHONEMIC_RING_SAMPLES * 2)
#define PHONEMIC_PERIOD_BYTES        (PHONEMIC_SAMPLE_RATE / 1000 * 5 * 2) // 5 мс = 480 Б
#define PHONEMIC_MAX_WAVERT_BUFFER   (PHONEMIC_SAMPLE_RATE * 2 * 2)        // до 2 c
#define PHONEMIC_MIN_WAVERT_BUFFER   (PHONEMIC_SAMPLE_RATE * 2 / 100)      // 20 мс

// ============================ extern C =====================================
extern "C" {
NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath);
}

// ============================ контрольное устройство ========================
NTSTATUS PhonemicControlDeviceCreate(_In_ PDRIVER_OBJECT DriverObject);
VOID PhonemicControlDeviceCleanup(VOID);
VOID PhonemicRingReset(VOID);

// доступ из miniport-потока: взять PCM из кольца (вызывается в DPC)
ULONG PhonemicRingPull(_Out_writes_bytes_(maxBytes) PUCHAR dest, _In_ ULONG maxBytes);
ULONG PhonemicRingBufferedBytes(VOID);
VOID PhonemicRingCountUnderrun(VOID);

// стандартный ответ BASICSUPPORT (реализован в mintopo.cpp; в WDK нет
// PcPropertyHandlerBasicSupport — этот хелпер заменяет его)
NTSTATUS PhonemicPropertyBasicSupport(_In_ PPCPROPERTY_REQUEST PropertyRequest);

// счётчик активного потока захвата (0/1)
extern LONG g_CaptureStreamActive;
