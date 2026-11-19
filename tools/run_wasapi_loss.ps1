[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Manifest,
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][string]$DisableFixtureScript,
    [Parameter(Mandatory)][string]$EnableFixtureScript
)
$ErrorActionPreference='Stop'
$fixture=& "$PSScriptRoot/validate_wasapi_fixture.ps1" -Manifest $Manifest -Executable $Executable -OutputDirectory $OutputDirectory
$prefix="Local\HailWasapi-$PID-$([Guid]::NewGuid().ToString('N'))"
$events=@{}
$process=$null
$disableAttempted=$false
try {
    foreach ($stage in 'ready','closed','restored') {
        $events[$stage]=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,"$prefix-$stage")
    }
    $arguments=@('--capture',$fixture.capture.key,'--playback',$fixture.playback.key,'--rate',"$($fixture.sample_rate)",
        '--channels',"$($fixture.channels)",'--pcm','f32','--unprimed','--loss-events',$prefix)
    $process=Start-Process -FilePath $Executable -ArgumentList $arguments -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $OutputDirectory 'loss.stdout.log') -RedirectStandardError (Join-Path $OutputDirectory 'loss.stderr.log')
    $null=$process.Handle # Retain ExitCode with Windows PowerShell redirection.
    if (!$events.ready.WaitOne(15000)) { throw 'Native loss trial did not become ready.' }
    # These explicitly supplied scripts implement the owner's approved mechanism.
    # Hail never guesses which device to disable or installs/reboots a driver.
    $disableAttempted=$true
    & $DisableFixtureScript -PnpInstanceId $fixture.pnp_instance_id
    if (!$?) { throw 'Provisioner disable procedure failed.' }
    if (!$events.closed.WaitOne(30000)) { throw 'Native loss/retirement did not complete.' }
    & $EnableFixtureScript -PnpInstanceId $fixture.pnp_instance_id
    if (!$?) { throw 'Provisioner restore procedure failed.' }
    $disableAttempted=$false
    $null=$events.restored.Set()
    if (!$process.WaitForExit(15000)) { throw 'Native reopen trial timed out.' }
    if ($process.ExitCode -ne 0) { throw "Native loss/reopen failed: $($process.ExitCode)" }
} finally {
    if ($disableAttempted) { & $EnableFixtureScript -PnpInstanceId $fixture.pnp_instance_id }
    if ($process -and !$process.HasExited) { Stop-Process -Id $process.Id }
    foreach ($event in $events.Values) { $event.Dispose() }
}
