# Сборка и подпись драйвера PhoneMic

Драйвер `phonemic.sys` — PortCls/WaveRT capture miniport (WDM). Собирается любым из способов ниже.
После сборки пакет: `phonemic.sys`, `phonemic.inf`, `phonemic.cat` (+ тестовая подпись).

## Способ 1. Visual Studio 2022 + WDK (рекомендуется)

1. Установите VS2022 с workload **Spectre/Mitigated libraries** не требуется; достаточно
   «Desktop development with C++».
2. Установите **WDK 10.0.22621+** (интегрируется в VS автоматически).
3. Соберите:
   ```powershell
   msbuild driver\phonemic.vcxproj /p:Configuration=Release /p:Platform=x64
   ```
   Артефакты: `driver\x64\Release\phonemic\{phonemic.sys, phonemic.inf, phonemic.cat}`.

## Способ 2. EWDK (без установки VS)

```powershell
# скачайте EWDK (ISO) с https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk
Mount-DiskImage -ImagePath .\EWDK_rs5.iso
D:\LaunchBuildEnv.cmd
  cd driver
  msbuild phonemic.vcxproj /p:Configuration=Release /p:Platform=x64
```

## Подпись

### Вариант A: тестовая подпись (для личного ПК)

```powershell
bcdedit /set testsigning on   # + перезагрузка
# при сборке WDK с включённым TestSign подписывает тестовым сертификатом автоматически;
# если нет — вручную:
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=PhoneMic Test" -CertStoreLocation Cert:\CurrentUser\My
signtool sign /a /fd SHA256 /ph /s CurrentUser /n "PhoneMic Test" driver\x64\Release\phonemic\phonemic.sys
signtool sign /a /fd SHA256 /ph /s CurrentUser /n "PhoneMic Test" driver\x64\Release\phonemic\phonemic.cat
```

В настройках появятся водяные знаки «Test Mode» — это нормально.

### Вариант B: attestation-подпись Microsoft (без HLK)

Если у организации есть EV-сертификат — подпишите cab и загрузите в Partner Center
(Device metadata / Driver attestation signing). Полученный подписанный cat ставится как
обычный драйвер, тестовая подпись не нужна. Подробно:
https://learn.microsoft.com/windows-hardware/drivers/dashboard/attestation-signing-a-kernel-driver-for-public-release

## Установка

```powershell
# от администратора:
powershell -ExecutionPolicy Bypass -File scripts\install_driver.ps1 -InfPath C:\path\to\phonemic.inf

# или вручную:
pnputil /add-driver phonemic.inf /install
devcon install phonemic.inf Root\PhoneMic      # devcon из WDK
```

Проверка: `Get-PnpDevice | Where-Object InstanceId -like "Root\PhoneMic*"`.
В «Звуковых устройствах» появится **PhoneMic Virtual Microphone**.

## Известные правки при первой сборке

Драйвер написан без доступа к реальному WDK-компилятору, поэтому возможны небольшие
расхождения сигнатур. Все потенциальные точки вынесены в одно место:

1. **`minwavertstream.cpp: AllocateBufferWithNotification`** — сигнатура
   `IMiniportWaveRTStreamNotification::AllocateBufferWithNotification`/`FreeBufferWithNotification`
   в вашей версии WDK может отличаться (число/порядок параметров). Если компилятор
   ругается на несоответствие — скопируйте точную сигнатуру из `portcls.h` WDK и
   перенаправьте вызов в `AllocateWaveRtBuffer`/`FreeWaveRtBuffer` (логика готова).
2. **`DEFINE_PCAUTOMATION_TABLE_PROP`** — если макрос определяет указатель, а не объект,
   замените `&WavePinAutomation` → `WavePinAutomation` (и аналогично в топологии).
3. **`minwavert.cpp: NewStream`** — приведение `(PUNKNOWN*)&stream` совместимо с
   `CUnknown`-фабриками; при необходимости замените на промежуточный `PUNKNOWN u`.
4. **INF** — при сборке через `Inf2Cat` убедитесь, что `CatalogFile` совпадает с архитектурой
   (`10_X64` уже прописан в vcxproj).

Всё остальное (кольцевой буфер, IOCTL, DPC-цикл, topology) не зависит от версии WDK.

## Отладка

```powershell
# Просмотр трассировки драйвера (DbgPrintEx, IHVDRIVER):
Install-Module DebugViewHelper -Scope CurrentUser   # либо Sysinternals DebugView (kernel capture)
# Проверка IOCTL-интерфейса:
Get-PnpDeviceProperty -InstanceId (Get-PnpDevice | ? InstanceId -like "Root\PhoneMic*" | % InstanceId)
```

Тест драйвера без телефона: WinUI-приложение → «Аудио» → **Тестовый сигнал** —
полоса микрофона в Windows/Zoom должна двигаться, `underruns` в «Драйвер» не расти.
