$ErrorActionPreference='Stop'
$out='docs/evidence/wasapi-paired-2026-11-18'
$capture='{0.0.1.00000000}.{87d84b71-17b7-4d13-89df-b4b95b98aa6d}'
$playback='{0.0.0.00000000}.{266bf420-96a2-47b5-a831-f6a6dbc0ae03}'
$owner='ROOT\MEDIA\0001'
$os=Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,BuildNumber,LastBootUpTime
$drivers=@(Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceName -eq 'VB-Audio Virtual Cable' } | Select-Object DeviceName,DeviceID,DriverVersion,InfName,IsSigned,Signer)
$pnp=foreach($id in @($owner,'ROOT\MEDIA\0002',"SWD\MMDEVAPI\$capture","SWD\MMDEVAPI\$playback")) {
 [ordered]@{device=Get-PnpDevice -InstanceId $id | Select-Object Status,Class,FriendlyName,InstanceId; properties=@(Get-PnpDeviceProperty -InstanceId $id | Where-Object {$_.KeyName -match 'Parent|ProblemCode|DriverInfPath|DriverVersion|Service|IsPresent'} | Select-Object KeyName,Data)}
}
$installed='C:/Windows/System32/DriverStore/FileRepository/vbmmecable64_win10.inf_amd64_9ed1e8995f3f9e21/vbaudio_cable64_win10.sys'
$package=(Resolve-Path 'build-wasapi-fixture/VBCABLE_Driver_Pack46.zip').Path
$sig=Get-AuthenticodeSignature -LiteralPath $installed
$config=foreach($leg in @(@{flow='Capture';guid='{87d84b71-17b7-4d13-89df-b4b95b98aa6d}'},@{flow='Render';guid='{266bf420-96a2-47b5-a831-f6a6dbc0ae03}'})) {
 $path="HKLM:/SOFTWARE/Microsoft/Windows/CurrentVersion/MMDevices/Audio/$($leg.flow)/$($leg.guid)"
 $fx=Get-Item -LiteralPath "$path/FxProperties"
 $props=Get-Item -LiteralPath "$path/Properties"
 [ordered]@{flow=$leg.flow;fx_value_names=@($fx.GetValueNames());properties=@(foreach($name in $props.GetValueNames()){[ordered]@{name=$name;value=$props.GetValue($name)}})}
}
[ordered]@{recorded_utc=[DateTimeOffset]::UtcNow.ToString('o');os=$os;drivers=$drivers;pnp=$pnp;configuration=$config;installed_driver_hash=(Get-FileHash $installed).Hash;installed_driver_signature=[string]$sig.Status;installed_signer_thumbprint=$sig.SignerCertificate.Thumbprint;source=(git rev-parse HEAD)} | ConvertTo-Json -Depth 12 | Set-Content "$out/provisioning.json"
$before=@(Get-Content docs/evidence/wasapi-fixture-install-2026-11-18/before-default-roles.jsonl)
$after=@(& ./build-windows-validation/OFF-Release/hail_wasapi_endpoint_level.exe --defaults)
if($LASTEXITCODE -ne 0 -or (Compare-Object $before $after)) {throw 'Default roles changed'}
$after | Set-Content "$out/default-roles.jsonl"
& ./build-windows-validation/OFF-Release/hail_wasapi_endpoint_level.exe '{0.0.1.00000000}.{bf16f049-5379-4d2e-a4d9-a15a142424d8}' | Set-Content "$out/shure-level.json"
if($sig.Status -ne 'Valid' -or (Get-FileHash $installed).Hash -ne '7EBCBBE7004D185F7B29613CD27FDC2DD71C1712224B767A74D2F66BDC33A7EF'){throw 'Installed driver mismatch'}
if(@($config | Where-Object {$_.fx_value_names.Count}).Count){throw 'Unexpected configured effects'}
$manifest=[ordered]@{
schema_version=1;profile='transparent-s16-grid';origin='https://vb-audio.com/Cable/';version='3.5.2.0';pnp_instance_id=$owner
owner_approved=$true;license_automation_approved=$true
license_terms='Owner-authorized standalone workflow evaluation only, per saved checkpoint and current user resumption. No purchase, redistribution or unattended driver installation approved. https://vb-audio.com/Services/licensing.htm'
installation_procedure='Owner installed staged signed package using standalone vendor setup and normal administrator consent; independently inspected installed driver and PnP ancestry after reboot.'
reboot_record="LastBootUpTime=$($os.LastBootUpTime.ToString('o')); later than pre-install checkpoint 2026-11-18T08:54:06Z"
exclusive=$true;reservation_owner='Hail qualification in this session; exact ROOT\MEDIA\0001 pair; no active sessions observed before reservation';reservation_expires_utc=[DateTimeOffset]::UtcNow.AddHours(3).ToString('o')
unity_gain=$true;effects_disabled=$true;sample_rate=48000;channels=2
package=@{path=$package;sha256=(Get-FileHash $package).Hash}
driver=@{path=$installed;sha256=(Get-FileHash $installed).Hash;signer_thumbprint=$sig.SignerCertificate.Thumbprint;inf_name='oem54.inf'}
capture=@{key=$capture;pnp_instance_id="SWD\MMDEVAPI\$capture"};playback=@{key=$playback;pnp_instance_id="SWD\MMDEVAPI\$playback"}
configuration_evidence='provisioning.json and capture-before/playback-unity.json: matching 48k stereo, unity master/channel gains, unmuted, no active sessions; empty endpoint FxProperties and no APO/effect registration in vendor INF. No physical monitoring configured by this session; native exact PCM remains mandatory.'
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content "$out/manifest-48000-2.json"
Copy-Item build-wasapi-fixture/inspect.cpp "$out/inspect.cpp"
& ./tools/validate_wasapi_fixture.ps1 -Manifest "$out/manifest-48000-2.json" -Executable ./build-windows-validation/OFF-Release/hail_wasapi_loopback.exe -OutputDirectory "$out/validation"
