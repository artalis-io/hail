[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$CaptureKey,
    [Parameter(Mandatory)][string]$PlaybackKey,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$exe=(Resolve-Path -LiteralPath $Executable).Path
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$output=(Resolve-Path -LiteralPath $OutputDirectory).Path
$helper=Join-Path (Split-Path -Parent $exe) 'hail_wasapi_endpoint_level.exe'
$original=& $helper $CaptureKey | ConvertFrom-Json
if ($LASTEXITCODE) { throw 'Cannot read Shure input settings' }
$original | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'input-settings-original.json')
$prefix="Local\HailPhysical-$PID-$([Guid]::NewGuid().ToString('N'))"
$events=@{}
$process=$null
try {
    & $helper $CaptureKey 0.5 0 | Set-Content -LiteralPath (Join-Path $output 'input-settings-test.json')
    if ($LASTEXITCODE) { throw 'Cannot set temporary Shure input level' }
    foreach ($stage in 'ready','closed','restored') {
        $events[$stage]=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,"$prefix-$stage")
    }
    $arguments=@('--physical','--capture',$CaptureKey,'--playback',$PlaybackKey,'--unprimed','--loss-events',$prefix)
    $process=Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $output 'hotplug.stdout.log') -RedirectStandardError (Join-Path $output 'hotplug.stderr.log')
    if (!$events.ready.WaitOne(15000)) { throw 'Physical stream did not become ready' }
    Write-Output 'READY: unplug the Shure USB cable now; reconnect after five seconds.'
    if (!$events.closed.WaitOne(30000)) { throw 'Physical removal/retirement was not observed within 30 seconds' }
    $deadline=[DateTimeOffset]::UtcNow.AddSeconds(25)
    do {
        $devices=@(& $exe --list | ForEach-Object { $_ | ConvertFrom-Json })
        $capture=@($devices | Where-Object { $_.key -ceq $CaptureKey -and $_.direction -eq 1 })
        $playback=@($devices | Where-Object { $_.key -ceq $PlaybackKey -and $_.direction -eq 2 })
        if ($LASTEXITCODE -eq 0 -and $capture.Count -eq 1 -and $playback.Count -eq 1) { break }
        Start-Sleep -Milliseconds 200
    } while ([DateTimeOffset]::UtcNow -lt $deadline)
    if ($capture.Count -ne 1 -or $playback.Count -ne 1) { throw 'Exact Shure endpoints did not return' }
    & $helper $CaptureKey 0.5 0 | Set-Content -LiteralPath (Join-Path $output 'input-settings-reconnected-test.json')
    if ($LASTEXITCODE) { throw 'Cannot apply test input level after reconnect' }
    $null=$events.restored.Set()
    if (!$process.WaitForExit(15000) -or $process.ExitCode -ne 0) { throw 'Physical reopen test failed' }
    Write-Output 'PASS physical device loss, retirement, explicit reopen and new incarnations'
} finally {
    if ($process -and !$process.HasExited) { Stop-Process -Id $process.Id }
    foreach ($event in $events.Values) { $event.Dispose() }
    $gain=([double]$original.gain).ToString('R',[Globalization.CultureInfo]::InvariantCulture)
    $mute=if ($original.muted) {'1'} else {'0'}
    & $helper $CaptureKey $gain $mute | Set-Content -LiteralPath (Join-Path $output 'input-settings-restored.json')
    if ($LASTEXITCODE) { throw 'Input settings could not be restored; reconnect the Shure and restore the retained original values' }
}
