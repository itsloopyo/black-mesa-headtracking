#!/usr/bin/env pwsh
#Requires -Version 5.1
# Thin wrapper - dev-deploy orchestration lives in
# cameraunlock-core/powershell/DevDeploy.psm1.

param(
    [Parameter(Position = 0)]
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [Parameter(Position = 1)]
    [string]$GivenPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot

Import-Module (Join-Path $projectRoot 'cameraunlock-core\powershell\DevDeploy.psm1') -Force

$buildOutput  = Join-Path $projectRoot "bin\$Configuration"
$vendorLoader = Join-Path $projectRoot 'vendor\ultimate-asi-loader\dinput8.dll'

# No ExeSubDir: the loader and the .asi both sit beside bms.exe at the game
# root. A copy in <game>\bin loads on a direct launch and NEVER under Steam -
# gameoverlayrenderer.dll is injected first and pulls WINMM.dll in from
# System32, and a base name already in the module list can no longer resolve
# to our proxy. winmm.dll is not a KnownDLL, so the exe directory is searched
# ahead of System32 and the root copy wins even that early load.
$result = Invoke-DevDeployASILoader `
    -GameId 'black-mesa' `
    -GameDisplayName 'Black Mesa' `
    -BuildOutputPath $buildOutput `
    -ModDllName 'BlackMesaHeadTracking.asi' `
    -VendorLoaderDll $vendorLoader `
    -AsiLoaderName 'winmm.dll' `
    -GivenPath $GivenPath

Write-Host ""
Write-Host "Deployed BlackMesaHeadTracking.asi to: $($result.ExeDir)" -ForegroundColor Green
Write-Host "Controls: End=toggle tracking, PgUp=cycle 6DOF/rotation/position, PgDn=toggle yaw mode." -ForegroundColor Gray
