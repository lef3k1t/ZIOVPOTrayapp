param(
    [string]$InstallDir = "$env:ProgramFiles\ZIOVPOTrayapp",
    [string]$ServiceName = "ZIOVPOTrayService"
)

$ErrorActionPreference = "Stop"

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "Run this installer from an elevated PowerShell session."
    }
}

Assert-Administrator

$SourceDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ServiceExe = Join-Path $InstallDir "ZIOVPOtrayservice.exe"
$TrayExe = Join-Path $InstallDir "ZIOVPOtrayapp.exe"
$DefaultDb = Join-Path $InstallDir "avdb-default.bin"
$ActiveDb = Join-Path $InstallDir "avdb.bin"

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

Copy-Item -Force (Join-Path $SourceDir "ZIOVPOtrayservice.exe") $ServiceExe
Copy-Item -Force (Join-Path $SourceDir "ZIOVPOtrayapp.exe") $TrayExe
Copy-Item -Force (Join-Path $SourceDir "avdb-default.bin") $DefaultDb

if (-not (Test-Path $ActiveDb)) {
    Copy-Item -Force $DefaultDb $ActiveDb
}

$existingService = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($existingService) {
    & sc.exe config $ServiceName binPath= "`"$ServiceExe`"" start= auto | Out-Null
} else {
    & sc.exe create $ServiceName binPath= "`"$ServiceExe`"" start= auto DisplayName= "ZIOVPO Tray Service" | Out-Null
}

& sc.exe failure $ServiceName reset= 86400 actions= restart/60000/restart/60000/""/60000 | Out-Null
Start-Service -Name $ServiceName

Write-Host "ZIOVPO Tray App installed to $InstallDir"
Write-Host "Service $ServiceName is configured for automatic startup."
