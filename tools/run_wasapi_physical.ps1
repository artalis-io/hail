[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$CaptureKey,
    [Parameter(Mandatory)][string]$PlaybackKey,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateSet('matrix','capture','playback','duplex','lifecycle')][string]$Mode='matrix',
    [ValidateRange(1,3600)][int]$Seconds=1,
    [ValidateRange(8000,192000)][int]$Rate=48000,
    [ValidateRange(1,2)][int]$Channels=2,
    [ValidateSet('s16','f32')][string]$Pcm='f32',
    [ValidateRange(0.0,1.0)][Nullable[double]]$TestCaptureGain,
    [switch]$Instrumented,
    [switch]$Unprimed
)
$ErrorActionPreference='Stop'
$exe=(Resolve-Path -LiteralPath $Executable).Path
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$output=(Resolve-Path -LiteralPath $OutputDirectory).Path
$devices=@(& $exe --list | ForEach-Object { $_ | ConvertFrom-Json })
if ($LASTEXITCODE) { throw 'WASAPI enumeration failed' }
foreach ($leg in @(@{key=$CaptureKey; direction=1},@{key=$PlaybackKey; direction=2})) {
    $matches=@($devices | Where-Object { $_.key -ceq $leg.key -and $_.direction -eq $leg.direction })
    if ($matches.Count -ne 1) { throw 'An explicitly selected physical endpoint is unavailable/ambiguous' }
}
$cases=@()
if ($Mode -eq 'matrix') {
    foreach ($testRate in 16000,24000,44100,48000) {
        foreach ($testChannels in 1,2) {
            foreach ($testPcm in 's16','f32') {
                foreach ($startup in 'primed','unprimed') {
                    $cases+=@{rate=$testRate; channels=$testChannels; pcm=$testPcm; startup=$startup; mode='duplex'}
                }
            }
        }
    }
} else { $cases+=@{rate=$Rate; channels=$Channels; pcm=$Pcm; startup=$(if ($Unprimed) {'unprimed'} else {'primed'}); mode=$Mode} }
$results=@()
$originalSettings=$null
$captureSettingChanged=$false
$levelHelper=Join-Path (Split-Path -Parent $exe) 'hail_wasapi_endpoint_level.exe'
try {
if ($null -ne $TestCaptureGain -and $Mode -ne 'playback') {
    $originalSettings=& $levelHelper $CaptureKey | ConvertFrom-Json
    if ($LASTEXITCODE) { throw 'Cannot read original capture settings' }
    $originalSettings | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'input-settings-original.json')
    $captureSettingChanged=$true
    $gainText=([double]$TestCaptureGain).ToString('R',[Globalization.CultureInfo]::InvariantCulture)
    & $levelHelper $CaptureKey $gainText 0 | Set-Content -LiteralPath (Join-Path $output 'input-settings-test.json')
    if ($LASTEXITCODE) { throw 'Cannot apply temporary test capture settings' }
}
foreach ($case in $cases) {
    $name="$($case.mode)-$($case.rate)-$($case.channels)-$($case.pcm)-$($case.startup)"
    $stdout=Join-Path $output "$name.stdout.log"
    $stderr=Join-Path $output "$name.stderr.log"
    if ((Test-Path -LiteralPath $stdout) -or (Test-Path -LiteralPath $stderr)) { throw "Refusing to overwrite trial evidence: $name" }
    $arguments=@('--physical','--seconds',"$Seconds",'--rate',"$($case.rate)",'--channels',"$($case.channels)",'--pcm',$case.pcm)
    if ($case.mode -ne 'playback') { $arguments+=@('--capture',$CaptureKey) }
    if ($case.mode -ne 'capture') { $arguments+=@('--playback',$PlaybackKey) }
    if ($case.startup -eq 'unprimed') { $arguments+='--unprimed' }
    if ($Instrumented) { $arguments+='--instrumented' }
    if ($case.mode -eq 'lifecycle') { $arguments+=@('--lifecycle-cycles','30') }
    $start=[DateTimeOffset]::UtcNow
    $process=Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $null=$process.Handle # Retain ExitCode with Windows PowerShell redirection.
    $samples=@()
    try {
        while (!$process.WaitForExit(1000)) {
            $process.Refresh()
            $samples+=@{utc=[DateTimeOffset]::UtcNow.ToString('o'); working_set=$process.WorkingSet64;
                private_bytes=$process.PrivateMemorySize64; handles=$process.HandleCount; cpu_seconds=$process.TotalProcessorTime.TotalSeconds}
            if (([DateTimeOffset]::UtcNow-$start).TotalSeconds -gt ($Seconds+90)) { throw "Trial timed out: $name" }
        }
        $process.Refresh()
        $text=Get-Content -LiteralPath $stdout -Raw
        $signal=if ($text -match 'meaningful_capture_minus90dbfs=([01])') { $Matches[1] -eq '1' } else { $null }
        $results+=@{name=$name; exit_code=$process.ExitCode; started_utc=$start.ToString('o');
            finished_utc=[DateTimeOffset]::UtcNow.ToString('o'); parameters=$case; process_samples=$samples;
            meaningful_signal=$signal;
            executable_sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash}
        $results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'summary.json') -Encoding utf8
        Write-Output "$name exit=$($process.ExitCode) meaningful_signal=$signal"
    } finally { if (!$process.HasExited) { Stop-Process -Id $process.Id } }
}
if (@($results | Where-Object exit_code -NE 0).Count) { throw 'Physical trials failed; all observations are retained in summary.json and raw logs.' }
} finally {
    if ($captureSettingChanged) {
        $gainText=([double]$originalSettings.gain).ToString('R',[Globalization.CultureInfo]::InvariantCulture)
        $muteText=if ($originalSettings.muted) {'1'} else {'0'}
        & $levelHelper $CaptureKey $gainText $muteText | Set-Content -LiteralPath (Join-Path $output 'input-settings-restored.json')
        if ($LASTEXITCODE) { throw 'Failed to restore the original capture settings; inspect the retained settings files' }
    }
}
