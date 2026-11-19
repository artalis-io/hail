# Device-free integration checks for the expensive native-runner gate.
param([string]$OutputDirectory='build-wasapi-runner-test')
$ErrorActionPreference='Stop'
$root=(Resolve-Path "$PSScriptRoot/..").Path
foreach($scenario in 'reject-unprimed','accept-f32') {
    $dir=Join-Path $OutputDirectory $scenario
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    Copy-Item "$root/tools/run_wasapi_qualification.ps1" "$dir/runner.ps1" -Force
    @'
param($Manifest,$Executable,$OutputDirectory)
[pscustomobject]@{sample_rate=48000;channels=2;profile='transparent-s16-grid-low-v1';capture=@{key='MOCK_CAPTURE'};playback=@{key='MOCK_RENDER'}}
'@ | Set-Content "$dir/validate_wasapi_fixture.ps1"
    @'
Write-Output 'MOCK conditioning only; no devices opened'
exit 0
'@ | Set-Content "$dir/condition.ps1"
    @'
if($args -contains 's16'){exit 1}
if(($PSScriptRoot -like '*reject-unprimed*') -and ($args -contains '--unprimed')){exit 1}
Write-Output 'MOCK native process only; no devices opened'
exit 0
'@ | Set-Content "$dir/native.ps1"
    $caught=$false
    try { & "$dir/runner.ps1" -Manifest 'MOCK' -Executable "$dir/native.ps1" -ConditioningExecutable "$dir/condition.ps1" -OutputDirectory $dir *> "$dir/invocation.log" }
    catch { $caught=$true }
    if(!$caught){throw 'Missing full-matrix profiles and S16 failures must reject aggregate'}
    $summary=Get-Content "$dir/summary.json" -Raw | ConvertFrom-Json
    if($scenario -eq 'reject-unprimed') {
        if($summary.failures -notcontains 'sustained-precondition' -or !(Test-Path "$dir/sustained.blocked.txt") -or (Test-Path "$dir/sustained.log")){throw 'Failed F32 startup must block sustained execution'}
    } else {
        if($summary.failures -contains 'sustained-precondition' -or !(Test-Path "$dir/sustained.log")){throw 'Passing F32 startup pair must allow separately scoped sustained execution despite S16 aggregate failure'}
    }
    Write-Output "PASS device-free qualification gate: $scenario"
}
