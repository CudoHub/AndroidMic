/*++
PhoneMic driver: ЕДИНСТВЕННЫЙ translation unit, где initguid.h раскрыт.
Здесь DEFINE_GUID из ks.h/ksmedia.h/portcls.h/phonemic_ioctl.h разворачиваются
в ОПРЕДЕЛЕНИЯ GUID. Все остальные .cpp получают только объявления.
--*/
#include <initguid.h>
#include "common.h"
