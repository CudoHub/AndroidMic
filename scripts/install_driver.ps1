#Requires -Version 5.1
<#
PhoneMic: установка драйвера виртуального микрофона.
Запускать от имени администратора:
    powershell -ExecutionPolicy Bypass -File install_driver.ps1 [-InfPath C:\path\to\driver]
#>
param(
    [string]$InfPath = ""
)

$ErrorActionPreference = "Stop"

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Write-Host "Запустите скрипт от имени администратора (правый клик -> PowerShell (Admin))." -ForegroundColor Red
        exit 1
    }
}
Assert-Admin

if (-not $InfPath) {
    # ищем phonemic.inf рядом со скриптом
    $here = Split-Path -Parent $MyInvocation.MyCommand.Path
    $candidates = @(
        (Join-Path $here "phonemic.inf"),
        (Join-Path $here "..\driver\phonemic.inf"),
        (Join-Path $here "driver\phonemic.inf")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $InfPath = (Resolve-Path $c).Path; break }
    }
}
if (-not $InfPath -or -not (Test-Path $InfPath)) {
    Write-Host "phonemic.inf не найден. Укажите -InfPath <путь>." -ForegroundColor Red
    exit 1
}
$dir = Split-Path -Parent $InfPath

Write-Host "=== PhoneMic: установка драйвера ===" -ForegroundColor Cyan
Write-Host "INF: $InfPath"

# 1. Тестовая подпись
$testSigning = (bcdedit /enum "{current}" | Select-String "testsigning Yes") -ne $null
if (-not $testSigning) {
    Write-Host @"
ВНИМАНИЕ: тестовая подпись выключена. Драйвер (тестовая подпись) не загрузится.
Включите её командой (от администратора) и перезагрузитесь:
    bcdedit /set testsigning on
Либо подпишите драйвер сертификатом с attestation-подписью Microsoft (см. docs/DRIVER_BUILD.md).
"@ -ForegroundColor Yellow
}

# 2. devcon?
$devcon = Get-ChildItem -Path $dir, "$dir\..\tools", "$env:ProgramFiles(x86)\Windows Kits\10\Tools\x64" -Filter "devcon.exe" -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName

# 3. Ставим драйвер в хранилище
Write-Host "[1/2] pnputil /add-driver ..."
& pnputil.exe /add-driver "$InfPath" /install | Write-Host

# 4. Создаём корневое устройство
Write-Host "[2/2] создание корневого устройства Root\PhoneMic ..."
$created = $false
if ($devcon) {
    & $devcon install "$InfPath" Root\PhoneMic
    $created = ($LASTEXITCODE -eq 0)
} else {
    Write-Host "devcon.exe не найден рядом со скриптом." -ForegroundColor Yellow
    Write-Host @"
Скачайте devcon (WDK) или используйте ручную команду:
    devcon install `"$InfPath`" Root\PhoneMic

Альтернатива без devcon (PowerShell, от администратора):
    pnputil /add-driver `"$InfPath`" /install
    # затем создать устройство:
    \$hw = Get-PnpDevice -Class Media | Out-Null   # (devcon надёжнее для корневых устройств)
"@ -ForegroundColor Gray
}

Start-Sleep -Seconds 2
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -like "Root\PhoneMic*" }
if ($dev) {
    Write-Host "Устройство установлено: $($dev.FriendlyName) [$($dev.Status)]" -ForegroundColor Green
    Write-Host "Проверьте микрофон «PhoneMic Virtual Microphone» в звуках Windows." -ForegroundColor Green
} else {
    Write-Host "Устройство не найдено после установки. Смотрите лог: $env:windir\INF\setupapi.dev.log" -ForegroundColor Yellow
    exit 1
}
