[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Manifest,
    [string]$Executable='build-windows-validation/OFF-Release/wasapi_native_repro.exe',
    [string]$ValidationExecutable='build-windows-validation/OFF-Release/hail_wasapi_loopback.exe',
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$output=(Resolve-Path $OutputDirectory).Path
if(Test-Path "$output/plan.json"){throw 'Use a fresh output directory; prior results must remain intact'}
$exe=(Resolve-Path $Executable).Path
$fixture=& "$PSScriptRoot/validate_wasapi_fixture.ps1" -Manifest $Manifest -Executable $ValidationExecutable -OutputDirectory "$output/validation"
if($fixture.sample_rate -ne 48000 -or $fixture.channels -ne 2 -or $fixture.profile -ne 'transparent-s16-grid-low-v1'){throw 'Native-only plan requires the approved low-grid 48 kHz stereo fixture'}
$cases=@(foreach($startup in 'primed','unprimed'){foreach($delay in 0,3,7,11){foreach($ack in 0,16){[ordered]@{name="$startup-$delay-$ack";unprimed=[int]($startup -eq 'unprimed');delay_ms=$delay;ack_delay_ms=$ack}}}})
[ordered]@{declared_utc=[DateTimeOffset]::UtcNow.ToString('o');source=(git rev-parse HEAD);working_tree=@(git status --short);executable_sha256=(Get-FileHash $exe).Hash;fixture=$fixture;cases=$cases;retries=0;sample_tolerance=0;native_process_links_hail=$false;native_process_conditions_its_own_silence=$true} |
    ConvertTo-Json -Depth 10 | Set-Content "$output/plan.json" -Encoding utf8
$results=[System.Collections.Generic.List[object]]::new()
foreach($case in $cases){
    $arguments=@($fixture.capture.key,$fixture.playback.key,"$($case.unprimed)","$($case.delay_ms)","$($case.ack_delay_ms)")
    $start=[DateTimeOffset]::UtcNow
    $process=Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput "$output/$($case.name).stdout.log" -RedirectStandardError "$output/$($case.name).stderr.log"
    $null=$process.Handle # Retain ExitCode with Windows PowerShell redirection.
    $timedOut=$false
    try {
        while(!$process.WaitForExit(1000)){if(([DateTimeOffset]::UtcNow-$start).TotalSeconds -gt 40){$timedOut=$true;$process.Kill();break}}
        $process.WaitForExit()
        $results.Add([ordered]@{name=$case.name;start_utc=$start.ToString('o');end_utc=[DateTimeOffset]::UtcNow.ToString('o');exit_code=$process.ExitCode;watchdog_expired=$timedOut;arguments=$arguments;stdout_sha256=(Get-FileHash "$output/$($case.name).stdout.log").Hash;stderr_sha256=(Get-FileHash "$output/$($case.name).stderr.log").Hash})
        $results | ConvertTo-Json -Depth 8 | Set-Content "$output/results.json" -Encoding utf8
        Write-Output "$($case.name) exit=$($process.ExitCode)"
    } finally {if(!$process.HasExited){$process.Kill();$process.WaitForExit()};$process.Dispose()}
}
$failures=@($results | Where-Object {$_.exit_code -ne 0 -or $_.watchdog_expired})
[ordered]@{completed_utc=[DateTimeOffset]::UtcNow.ToString('o');planned_cases=$cases.Count;executed_cases=$results.Count;failed_cases=@($failures | ForEach-Object {$_.name});hail_qualification='not performed by native-only program';physical_latency='not measured'} |
    ConvertTo-Json -Depth 5 | Set-Content "$output/summary.json" -Encoding utf8
if($failures.Count){throw "$($failures.Count) native-only cases rejected exact PCM; results retained; no retries"}
