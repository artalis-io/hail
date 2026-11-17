[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Manifest,
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$fixture = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
if ($fixture.schema_version -ne 1 -or $fixture.profile -ne 'transparent-s16-grid') {
    throw 'Require manifest schema 1 and the predeclared transparent-s16-grid oracle.'
}
if ($fixture.sample_rate -notin 16000,24000,44100,48000 -or $fixture.channels -notin 1,2) {
    throw 'Manifest must declare a supported qualification rate and mono/stereo channels.'
}
foreach ($name in 'origin','version','license_terms','installation_procedure','reboot_record','reservation_owner') {
    if ([string]::IsNullOrWhiteSpace($fixture.$name)) { throw "Missing $name" }
}
if ($fixture.owner_approved -ne $true -or $fixture.license_automation_approved -ne $true -or
    $fixture.exclusive -ne $true -or $fixture.unity_gain -ne $true -or $fixture.effects_disabled -ne $true) {
    throw 'Owner/license approval, exclusive reservation, unity gains and disabled effects are required.'
}
if ([DateTimeOffset]::Parse($fixture.reservation_expires_utc) -le [DateTimeOffset]::UtcNow) {
    throw 'Fixture reservation has expired.'
}
foreach ($name in 'package','driver') {
    $file = $fixture.$name
    if ($file.sha256 -notmatch '^[0-9a-fA-F]{64}$') { throw "Invalid $name hash" }
    $actual = Get-FileHash -LiteralPath $file.path -Algorithm SHA256
    if ($actual.Hash -ne $file.sha256) { throw "$name hash mismatch" }
}
$signature = Get-AuthenticodeSignature -LiteralPath $fixture.driver.path
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $fixture.driver.signer_thumbprint) {
    throw 'Driver signature or pinned signer mismatch.'
}
$owner = Get-PnpDevice -InstanceId $fixture.pnp_instance_id
if ($owner.Status -ne 'OK') { throw 'Fixture PnP owner is unavailable.' }
$drivers = @(Get-CimInstance Win32_PnPSignedDriver | Where-Object DeviceID -EQ $fixture.pnp_instance_id)
if ($drivers.Count -ne 1 -or !$drivers[0].IsSigned -or $drivers[0].DriverVersion -ne $fixture.version -or
    $drivers[0].InfName -ne $fixture.driver.inf_name) { throw 'Installed driver identity/version/signing mismatch.' }
$devices = @(& $Executable --list | ForEach-Object { $_ | ConvertFrom-Json })
if ($LASTEXITCODE -ne 0) { throw 'Hail WASAPI enumeration failed.' }
foreach ($leg in @(@{name='capture'; direction=1}, @{name='playback'; direction=2})) {
    $endpoint = $fixture.($leg.name)
    $matches = @($devices | Where-Object { $_.key -ceq $endpoint.key -and $_.direction -eq $leg.direction })
    if ($matches.Count -ne 1) { throw "Exact $($leg.name) Hail persistent identity/direction unavailable." }
    if ($endpoint.pnp_instance_id -ine ('SWD\MMDEVAPI\' + $endpoint.key)) { throw 'Endpoint PnP/key mismatch.' }
    $device = Get-PnpDevice -InstanceId $endpoint.pnp_instance_id
    if ($device.Status -ne 'OK' -or $device.Class -ne 'AudioEndpoint') { throw 'Endpoint is not active.' }
    # Follow authoritative PnP ancestry. A similar friendly name never proves ownership.
    $current = $device.InstanceId
    $found = $false
    for ($depth=0; $depth -lt 16; $depth++) {
        if ($current -ieq $fixture.pnp_instance_id) { $found=$true; break }
        $parent = Get-PnpDeviceProperty -InstanceId $current -KeyName DEVPKEY_Device_Parent
        if (!$parent.Data -or $parent.Data -ieq $current) { break }
        $current = $parent.Data
    }
    if (!$found) { throw "Cannot prove $($leg.name) ownership by manifest PnP ancestry." }
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$record = [ordered]@{
    validated_utc = [DateTimeOffset]::UtcNow.ToString('o')
    manifest_sha256 = (Get-FileHash -LiteralPath $Manifest -Algorithm SHA256).Hash
    executable_sha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash
    fixture = $fixture
    signature_status = [string]$signature.Status
    installed_driver = $drivers | Select-Object DeviceID,DriverVersion,InfName,IsSigned,Signer
    hail_endpoints = $devices
    os = Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,BuildNumber,OSArchitecture
    cpu = Get-CimInstance Win32_Processor | Select-Object Name,NumberOfLogicalProcessors
    services = Get-Service Audiosrv,AudioEndpointBuilder | Select-Object Name,Status
    power = powercfg /getactivescheme
    source = git rev-parse HEAD
    working_tree = @(git status --short)
}
$record | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'fixture-validation.json') -Encoding utf8
$fixture
