[CmdletBinding()]
param(
    [string]$BuildDirectory='build-windows-validation',
    [string]$AsanDirectory='build-windows-asan',
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$artifacts=@()
foreach ($shared in 'OFF','ON') {
    foreach ($config in 'Debug','Release') {
        $name="$shared-$config"
        $source=Join-Path $BuildDirectory $name
        $destination=Join-Path $OutputDirectory $name
        New-Item -ItemType Directory -Path $destination -Force | Out-Null
        Compress-Archive -LiteralPath (Join-Path $source 'CMakeCache.txt') -DestinationPath (Join-Path $destination 'CMakeCache.zip') -Force
        foreach ($file in 'configure.log','build.log','ctest.log','contracts.log','endpoints.jsonl','consumer-ctest.log') {
            Copy-Item -LiteralPath (Join-Path $source $file) -Destination (Join-Path $destination $file)
        }
        foreach ($file in 'hail_wasapi_loopback.exe',$(if ($shared -eq 'ON') {'hail.dll'} else {'hail.lib'})) {
            $artifacts+=Get-FileHash -LiteralPath (Join-Path $source $file) -Algorithm SHA256 | Select-Object Path,Hash
        }
    }
}
if (Test-Path -LiteralPath "$AsanDirectory/Testing/Temporary/LastTest.log") {
    Copy-Item -LiteralPath "$AsanDirectory/Testing/Temporary/LastTest.log" -Destination (Join-Path $OutputDirectory 'asan-initialization-failure.log')
    Compress-Archive -LiteralPath "$AsanDirectory/CMakeCache.txt" -DestinationPath (Join-Path $OutputDirectory 'asan-CMakeCache.zip') -Force
    $artifacts+=Get-FileHash -LiteralPath "$AsanDirectory/hail_wasapi_buffers.exe" -Algorithm SHA256 | Select-Object Path,Hash
}
$sources=@(rg --files src include tests tools cmake .github | Sort-Object | ForEach-Object {
    [ordered]@{path=$_; sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash}
})
[ordered]@{
    collected_utc=[DateTimeOffset]::UtcNow.ToString('o')
    base_revision=git rev-parse HEAD
    branch=git branch --show-current
    working_tree=@(git status --short)
    source_hashes=$sources
    artifact_hashes=$artifacts
    os=Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,BuildNumber,OSArchitecture
    cpu=Get-CimInstance Win32_Processor | Select-Object Name,NumberOfLogicalProcessors
    compiler=@(& clang-cl --version 2>&1 | ForEach-Object { "$_" })
    cmake=@(& cmake --version)
    ninja=& ninja --version
    power=@(& powercfg /getactivescheme)
    audio_services=Get-Service Audiosrv,AudioEndpointBuilder | Select-Object Name,Status
    endpoints=Get-PnpDevice -Class AudioEndpoint | Select-Object Status,Class,FriendlyName,InstanceId
    audio_drivers=Get-CimInstance Win32_PnPSignedDriver | Where-Object DeviceClass -EQ 'MEDIA' |
        Select-Object DeviceName,DeviceID,DriverVersion,InfName,IsSigned,Signer
    default_thread_policy='No Hail MMCSS/priority/power changes; ordinary application and worker threads'
    native_streams_executed=$false
    asan='Initial implementation build: matching LLVM 19 runtime failed before eight then-existing tests; not final-tree sanitizer execution'
    application_verifier='Installed; not enabled because current shell is not elevated'
} | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'environment-and-hashes.json') -Encoding utf8
