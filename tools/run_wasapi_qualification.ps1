[CmdletBinding()]
param(
    [Parameter(Mandatory)][string[]]$Manifest,
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$ConditioningExecutable,
    [ValidateRange(600,3600)][int]$SustainedSeconds=600
)
$ErrorActionPreference = 'Stop'
if (!$ConditioningExecutable) { $ConditioningExecutable=Join-Path (Split-Path $Executable -Parent) 'hail_wasapi_path.exe' }
if (!(Test-Path -LiteralPath $ConditioningExecutable)) { throw 'Require the native path helper for explicit fixture silence conditioning.' }
function Invoke-Condition($Fixture,[string]$Name) {
    # Failed trials may leave nonzero downstream cable buffers. This separate
    # preflight drives/observes exact silence before a fresh dynamic trial;
    # it never resets or forgives the qualification oracle inside that trial.
    & $ConditioningExecutable $Fixture.capture.key $Fixture.playback.key f32 --condition 2>&1 |
        Tee-Object -FilePath (Join-Path $OutputDirectory "$Name.conditioning.log")
    if ($LASTEXITCODE -ne 0) { throw "Fixture conditioning failed before $Name; no retry attempted." }
}
$fixtures = @()
for ($index=0; $index -lt $Manifest.Count; $index++) {
    $fixtures += & "$PSScriptRoot/validate_wasapi_fixture.ps1" -Manifest $Manifest[$index] -Executable $Executable `
        -OutputDirectory (Join-Path $OutputDirectory "fixtures/$index")
}
$failures = [System.Collections.Generic.List[string]]::new()
foreach ($rate in 16000,24000,44100,48000) {
    foreach ($channels in 1,2) {
        foreach ($pcm in 's16','f32') {
            $name = "$rate-$channels-$pcm"
            $profiles = @($fixtures | Where-Object { $_.sample_rate -eq $rate -and $_.channels -eq $channels })
            # A transparent oracle requires the SAME application and fixture rates.
            # Conversion needs its own numerical oracle; never accept arbitrary PCM.
            if (!$profiles.Count) {
                "UNTESTED $name : provision a matching transparent fixture profile, or implement a predeclared conversion oracle." |
                    Set-Content -LiteralPath (Join-Path $OutputDirectory "$name.unqualified.txt")
                $failures.Add($name)
                continue
            }
            if ($profiles.Count -ne 1) { throw "Ambiguous fixture profile for $name" }
            $fixture=$profiles[0]
            foreach ($startup in 'primed','unprimed') {
                Invoke-Condition $fixture "$name-$startup"
                $arguments = @('--capture',$fixture.capture.key,'--playback',$fixture.playback.key,
                    '--rate',"$rate",'--channels',"$channels",'--pcm',$pcm,'--seconds','5')
                if ($startup -eq 'unprimed') { $arguments += '--unprimed' }
                if ($fixture.profile -eq 'transparent-s16-grid-low-v1') { $arguments += '--low-grid' }
                & $Executable @arguments 2>&1 | Tee-Object -FilePath (Join-Path $OutputDirectory "$name-$startup.log")
                if ($LASTEXITCODE -ne 0) { $failures.Add("$name-$startup") }
            }
        }
    }
}
$fixture=$fixtures[0]
Invoke-Condition $fixture 'sustained'
$arguments = @('--capture',$fixture.capture.key,'--playback',$fixture.playback.key,
    '--rate',"$($fixture.sample_rate)",'--channels',"$($fixture.channels)",'--pcm','f32','--seconds',"$SustainedSeconds")
if ($fixture.profile -eq 'transparent-s16-grid-low-v1') { $arguments += '--low-grid' }
& $Executable @arguments 2>&1 | Tee-Object -FilePath (Join-Path $OutputDirectory 'sustained.log')
if ($LASTEXITCODE -ne 0) { $failures.Add('sustained') }
[ordered]@{completed_utc=[DateTimeOffset]::UtcNow.ToString('o'); failures=@($failures);
    endpoint_disable_reenable='not executed by this harness; provisioner-controlled lifecycle trial required';
    physical_audio='not exercised by paired virtual fixture'; physical_latency='not measured'} |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json')
if ($failures.Count) { throw "Qualification incomplete/failed: $($failures -join ', '). Raw evidence retained." }
