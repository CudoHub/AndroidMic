# PhoneMic Driver API — интерфейс взаимодействия WinUI-приложения с драйвером

Драйвер `phonemic.sys` — PortCls/WaveRT capture miniport, создающий системный микрофон
«PhoneMic Virtual Microphone». WinUI-приложение подаёт PCM в драйвер через control-устройство.

## 1. Идентификация

| Параметр | Значение |
|---|---|
| Имя службы | `phonemic` |
| Enumerator / Hardware ID | `Root\PhoneMic` |
| Device interface GUID | `7C0A9E52-3B14-4D6F-9A8B-2E5C1D3F7A01` |
| Legacy symbolic link | `\DosDevices\PhoneMic` (=> `\\.\PhoneMic`) |
| Friendly name устройства | `PhoneMic Virtual Microphone` |
| Класс устройства | MEDIA `{4d36e96c-e325-11ce-bfc1-08002be10318}` |

Поиск: `SetupDiGetClassDevs(&GUID_DEVINTERFACE_PHONEMIC)` → первый `SPDRP_DEVICEINTERFACE` путь
с включённым состоянием. Фолбэк — `CreateFileW(L"\\\\.\\PhoneMic", ...)`.

## 2. IOCTL (все — METHOD_BUFFERED, FILE_ANY_ACCESS, FILE_DEVICE_UNKNOWN)

| Код | Функция | Направление | Вход | Выход |
|---|---|---|---|---|
| 0x800 | `IOCTL_PHONEMIC_GET_VERSION` | OUT | — | `ULONG version` (= 1) |
| 0x801 | `IOCTL_PHONEMIC_PUSH_SAMPLES` | IN | PCM16LE mono 48 kHz (произвольный размер чанка ≤ 65536) | `ULONG acceptedBytes` |
| 0x802 | `IOCTL_PHONEMIC_GET_STATE` | OUT | — | `PHONEMIC_STATE` (32 байта, см. ниже) |
| 0x803 | `IOCTL_PHONEMIC_SET_ACTIVE` | IN | `ULONG` (0/1) | — |
| 0x804 | `IOCTL_PHONEMIC_FLUSH` | — | — | — |

```c
typedef struct _PHONEMIC_STATE {
    ULONG version;          // 1
    ULONG active;           // приложение подаёт данные (не тишина)
    ULONG underruns;        // счётчик недоборов с момента старта потока
    ULONG overflows;        // счётчик переполнений кольца
    ULONG bufferedBytes;    // заполнение кольца, байт
    ULONG ringBytes;        // размер кольца, байт (9600)
    ULONG periodMicrosec;   // период DPC, мкс (5000)
    ULONG sampleRate;       // 48000
} PHONEMIC_STATE;
```
Layout: 8 × `ULONG` = 32 байта, без выравнивающих дыр. C#-зеркало — `[StructLayout(LayoutKind.Sequential)]`.

## 3. Поведение

- Кольцевой буфер драйвера: **9600 байт = 100 мс** (4800 сэмплов), nonpaged pool, защита spinlock.
- `PUSH_SAMPLES`: `memcpy` в кольцо, возвращает фактически принятые байты. Если кольцо заполнено — принимает частично; приложение должно следить за `bufferedBytes` и не заливать вперёд более чем на ~40 мс.
- Период DPC — 5 мс. На каждом тике при `RUN` потока драйвер копирует из кольца в WaveRT-циклический буфер до 5 мс данных; при нехватке — тишину и `underruns++`. При `active=0` — всегда тишина.
- `FLUSH` обнуляет кольцо и счётчики underrun/overflow.
- Если KS-поток захвата не открыт (никто не слушает микрофон) — копирование не выполняется, кольцо копится/переполняется: это нормально, `overflows++` в этом случае не является ошибкой.

## 4. KS-интерфейс (для аудиодвижка Windows)

- Pin захвата: `KSNODETYPE_MICROPHONE`, `KSPIN_DATAFLOW_OUT`, единственный инстанс.
- Форматы: PCM 16-bit mono 48 кГц (основной), PCM 16-bit mono 44.1 кГц (резерв).
- WaveRT: циклический буфер, event-driven mode (KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT), position register, clock register.
- Топология: `MIC` (bridge) → `VOLUME` → `MUTE` → wave pin. Jack description: 3.5 мм combo.

## 5. Установка / удаление

```powershell
# установка (тестовая подпись должна быть включена)
devcon.exe install phonemic.inf Root\PhoneMic
# либо
pnputil /add-driver phonemic.inf /install; затем создать корневое устройство:
devcon.exe install phonemic.inf Root\PhoneMic

# удаление
devcon.exe remove Root\PhoneMic
pnputil /delete-driver phonemic.inf /uninstall
```

Скрипты: `scripts/install_driver.ps1`, `scripts/uninstall_driver.ps1`.
Состояние и счётчики драйвера отображаются на странице «Драйвер» в WinUI-приложении.
