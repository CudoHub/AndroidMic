#Requires -Version 5.1
<#
PhoneMic: удаление драйвера виртуального микрофона. Запуск от администратора.
#>
$ErrorActionPreference = "Continue"
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Запустите скрипт от имени администратора." -ForegroundColor Red
    exit 1
}

Write-Host "=== PhoneMic: удаление драйвера ===" -ForegroundColor Cyan

$devcon = Get-ChildItem -Path (Split-Path -Parent $MyInvocation.MyCommand.Path) -Filter "devcon.exe" -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName

if ($devcon) {
    & $devcon remove Root\PhoneMic
} else {
    $dev = Get-PnpDevice | Where-Object { $_.InstanceId -like "Root\PhoneMic*" }
    if ($dev) {
        foreach ($d in $dev) {
            Write-Host "Удаляем $($d.InstanceId)..."
            & pnputil.exe /remove-device "$($d.InstanceId)"
        }
    }
}

& pnputil.exe /delete-driver phonemic.inf /uninstall /force

Write-Host "Готово. Если устройство оставалось — перезагрузите ПК." -ForegroundColor Green
