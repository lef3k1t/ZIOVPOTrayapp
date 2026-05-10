param(
    [string]$InstallDir = "$env:ProgramFiles\ZIOVPOTrayapp",
    [string]$ServiceName = "ZIOVPOTrayService"
)

$ErrorActionPreference = "Stop"

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "Run this uninstaller from an elevated PowerShell session."
    }
}

Assert-Administrator

$service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($service) {
    try {
        Stop-Service -Name $ServiceName -Force -ErrorAction Stop
        $service.WaitForStatus("Stopped", [TimeSpan]::FromSeconds(15))
    } catch {
        $serviceProcessId = (Get-CimInstance Win32_Service -Filter "Name='$ServiceName'").ProcessId
        if ($serviceProcessId -and $serviceProcessId -ne 0) {
            Stop-Process -Id $serviceProcessId -Force -ErrorAction SilentlyContinue
        }
        Stop-Process -Name "ZIOVPOtrayapp" -Force -ErrorAction SilentlyContinue
    }

    & sc.exe delete $ServiceName | Out-Null
}

Stop-Process -Name "ZIOVPOtrayapp" -Force -ErrorAction SilentlyContinue
Stop-Process -Name "ZIOVPOtrayservice" -Force -ErrorAction SilentlyContinue

if (Test-Path $InstallDir) {
    Remove-Item -LiteralPath $InstallDir -Recurse -Force
}

Write-Host "ZIOVPO Tray App was removed."
