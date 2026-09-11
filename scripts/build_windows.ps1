#Requires -Version 5.1
<#
PhoneMic: сборка Windows-приложения (WinUI 3) + драйвера.
    .\build_windows.ps1 [-Driver]   # только приложение по умолчанию; -Driver — ещё и драйвер
Требуется: .NET SDK 8, (для драйвера) WDK 10.0.22621+ и Visual Studio 2022/Build Tools.
#>
param([switch]$Driver)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$out = Join-Path $root "artifacts"
New-Item -ItemType Directory -Force -Path $out | Out-Null

Write-Host "=== PhoneMic: сборка Windows-приложения ===" -ForegroundColor Cyan

dotnet --version | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "Не найден .NET SDK 8 (dotnet)." -ForegroundColor Red; exit 1 }

$proj = Join-Path $root "windows-app\src\PhoneMic.App\PhoneMic.App.csproj"
dotnet publish $proj -c Release -r win-x64 --self-contained `
    -p:Platform=x64 -p:WindowsAppSDKSelfContained=true
if ($LASTEXITCODE -ne 0) { Write-Host "Сборка приложения не удалась." -ForegroundColor Red; exit 1 }

$publishDir = Join-Path (Split-Path -Parent $proj) "bin\Release\net8.0-windows10.0.22621.0\win-x64\publish"
if (Test-Path $publishDir) {
    $zip = Join-Path $out "PhoneMic-win-x64.zip"
    Compress-Archive -Path "$publishDir\*" -DestinationPath $zip -Force
    Write-Host "Приложение упаковано: $zip" -ForegroundColor Green
}

if ($Driver) {
    Write-Host "=== Сборка драйвера (требуется WDK) ===" -ForegroundColor Cyan
    $vcxproj = Join-Path $root "driver\phonemic.vcxproj"
    $msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1
    if (-not $msbuild) { Write-Host "MSBuild не найден (нужен VS2022/Build Tools + WDK)." -ForegroundColor Red; exit 1 }
    & $msbuild $vcxproj /p:Configuration=Release /p:Platform=x64 /t:Build
    if ($LASTEXITCODE -ne 0) { Write-Host "Сборка драйвера не удалась (см. docs/DRIVER_BUILD.md)." -ForegroundColor Red; exit 1 }
    $drvOut = Join-Path $root "driver\x64\Release\phonemic"
    Copy-Item "$drvOut\phonemic.sys", "$drvOut\phonemic.inf", "$drvOut\phonemic.cat" $out -Force -ErrorAction SilentlyContinue
    Write-Host "Драйвер в: $out" -ForegroundColor Green
}
