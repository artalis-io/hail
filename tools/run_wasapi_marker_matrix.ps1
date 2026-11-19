[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Manifest,
    [string]$Executable='build-windows-validation/OFF-Release/hail_wasapi_loopback.exe',
    [string]$ConditioningExecutable='build-windows-validation/OFF-Release/hail_wasapi_path.exe',
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$SdkBoundaryMatrix,
    [switch]$RegressionCases
)
$ErrorActionPreference='Stop'
if($RegressionCases -and $SdkBoundaryMatrix){throw 'Choose one predeclared matrix'}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$output=(Resolve-Path $OutputDirectory).Path
if(Test-Path "$output/plan.json"){throw 'Use a new evidence directory; prior trial results must not be overwritten'}
$executablePath=(Resolve-Path $Executable).Path
$conditioningPath=(Resolve-Path $ConditioningExecutable).Path
$fixture=& "$PSScriptRoot/validate_wasapi_fixture.ps1" -Manifest $Manifest -Executable $executablePath -OutputDirectory "$output/validation"
if($fixture.profile -ne 'transparent-s16-grid-low-v1' -or $fixture.sample_rate -ne 48000 -or $fixture.channels -ne 2){throw 'This predeclared matrix requires the low-grid 48 kHz stereo fixture'}
$tapModes=if($SdkBoundaryMatrix){@('none')}else{@('none','capture','both')}
$delays=if($SdkBoundaryMatrix){@(0,3,7,11)}else{@(0,7)}
$plan=@(foreach($tap in $tapModes){foreach($startup in 'primed','unprimed'){foreach($slots in 8,32){foreach($delay in $delays){
    [ordered]@{name="$tap-$startup-$slots-$delay";tap=$tap;startup=$startup;slots=$slots;delay_ms=$delay;seconds=5}
}}}})
if($RegressionCases) {
    $plan=@(
        [ordered]@{name='none-primed-8-3';tap='none';startup='primed';slots=8;delay_ms=3;seconds=5},
        [ordered]@{name='none-primed-32-7';tap='none';startup='primed';slots=32;delay_ms=7;seconds=5}
    )
}
[ordered]@{declared_utc=[DateTimeOffset]::UtcNow.ToString('o');source=(git rev-parse HEAD);working_tree=@(git status --short);sdk_boundary_matrix=[bool]$SdkBoundaryMatrix;regression_cases=[bool]$RegressionCases;executable_sha256=(Get-FileHash $executablePath).Hash;conditioning_sha256=(Get-FileHash $conditioningPath).Hash;fixture=$fixture;cases=$plan;retry='none';sample_tolerance=0;clock_pair_alignment_ns=100} |
    ConvertTo-Json -Depth 10 | Set-Content "$output/plan.json" -Encoding utf8
function Invoke-Observed([string]$Program,[string[]]$Arguments,[string]$Name,[int]$TimeoutSeconds) {
    $start=[DateTimeOffset]::UtcNow
    $process=Start-Process -FilePath $Program -ArgumentList $Arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput "$output/$Name.stdout.log" -RedirectStandardError "$output/$Name.stderr.log"
    # Windows PowerShell can lose ExitCode for redirected, uncached handles.
    $null=$process.Handle
    $timedOut=$false
    try {
        while(!$process.WaitForExit(1000)) {
            if(([DateTimeOffset]::UtcNow-$start).TotalSeconds -gt $TimeoutSeconds){$timedOut=$true;$process.Kill();break}
        }
        $process.WaitForExit()
        if($null -eq $process.ExitCode){throw 'Child exit code unavailable; trial cannot be classified'}
        [ordered]@{name=$Name;start_utc=$start.ToString('o');end_utc=[DateTimeOffset]::UtcNow.ToString('o');exit_code=$process.ExitCode;watchdog_expired=$timedOut;arguments=$Arguments;stdout_sha256=(Get-FileHash "$output/$Name.stdout.log").Hash;stderr_sha256=(Get-FileHash "$output/$Name.stderr.log").Hash}
    } finally { if(!$process.HasExited){$process.Kill();$process.WaitForExit()};$process.Dispose() }
}
$results=[System.Collections.Generic.List[object]]::new()
foreach($case in $plan) {
    $condition=Invoke-Observed $conditioningPath @($fixture.capture.key,$fixture.playback.key,'f32','--condition') "$($case.name).condition" 20
    $results.Add($condition)
    if($condition.exit_code -ne 0 -or $condition.watchdog_expired){$results | ConvertTo-Json -Depth 8 | Set-Content "$output/results.json" -Encoding utf8;throw 'Conditioning failed; remaining cases not attempted'}
    $arguments=@('--capture',$fixture.capture.key,'--playback',$fixture.playback.key,'--low-grid','--pcm','f32','--seconds','5','--marker-taps',$case.tap,'--playback-slots',"$($case.slots)",'--flush-delay-ms',"$($case.delay_ms)")
    if($case.startup -eq 'unprimed'){$arguments+='--unprimed'}
    $result=Invoke-Observed $executablePath $arguments $case.name 40
    $results.Add($result)
    $results | ConvertTo-Json -Depth 8 | Set-Content "$output/results.json" -Encoding utf8
    Write-Output "$($case.name) exit=$($result.exit_code)"
}
$failed=@($results | Where-Object {$_.exit_code -ne 0 -or $_.watchdog_expired})
[ordered]@{completed_utc=[DateTimeOffset]::UtcNow.ToString('o');planned_cases=$plan.Count;executed_cases=@($results | Where-Object {$_.name -notlike '*.condition'}).Count;failures=@($failed | ForEach-Object {$_.name});complete_qualification=$false;physical_latency='not measured'} |
    ConvertTo-Json -Depth 5 | Set-Content "$output/summary.json" -Encoding utf8
if($failed.Count){throw "$($failed.Count) predeclared cases failed; all results retained; no retries"}
