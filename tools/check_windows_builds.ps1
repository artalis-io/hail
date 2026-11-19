[CmdletBinding()]
param([string]$OutputDirectory='build-windows-validation')
$ErrorActionPreference='Stop'
$root=(Resolve-Path "$PSScriptRoot/..").Path
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$output=(Resolve-Path $OutputDirectory).Path
function Invoke-Checked([string]$Program,[string[]]$Arguments,[string]$Log) {
    & $Program @Arguments *> $Log
    if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath $Log -Tail 30; throw "$Program failed: $Log" }
}
foreach ($shared in 'OFF','ON') {
    foreach ($config in 'Debug','Release') {
        $name="$shared-$config"
        $build=Join-Path $output $name
        New-Item -ItemType Directory -Path $build -Force | Out-Null
        Invoke-Checked cmake @('-S',$root,'-B',$build,'-G','Ninja','-DCMAKE_C_COMPILER=clang-cl',
            '-DCMAKE_CXX_COMPILER=clang-cl',"-DCMAKE_BUILD_TYPE=$config","-DHA_BUILD_SHARED=$shared",'-DHA_BUILD_NATIVE_TESTS=ON') (Join-Path $build 'configure.log')
        Invoke-Checked cmake @('--build',$build,'--parallel') (Join-Path $build 'build.log')
        Invoke-Checked ctest @('--test-dir',$build,'--output-on-failure') (Join-Path $build 'ctest.log')
        $library=Join-Path $build 'hail.lib'
        $check=@("$root/tools/check_contracts.py",$library)
        if ($shared -eq 'ON') { $check=@("$root/tools/check_contracts.py",(Join-Path $build 'hail.dll'),'--shared') }
        Invoke-Checked python $check (Join-Path $build 'contracts.log')
        Invoke-Checked (Join-Path $build 'hail_wasapi_loopback.exe') @('--list') (Join-Path $build 'endpoints.jsonl')
        $prefix=Join-Path $build 'installed'
        Invoke-Checked cmake @('--install',$build,'--prefix',$prefix) (Join-Path $build 'install.log')
        $consumer=Join-Path $build 'consumer'
        Invoke-Checked cmake @('-S',"$root/tests/consumer",'-B',$consumer,'-G','Ninja',
            '-DCMAKE_C_COMPILER=clang-cl','-DCMAKE_CXX_COMPILER=clang-cl',"-DCMAKE_BUILD_TYPE=$config", "-DCMAKE_PREFIX_PATH=$prefix",'-DHA_CONSUMER_DSP=ON') (Join-Path $build 'consumer-configure.log')
        Invoke-Checked cmake @('--build',$consumer,'--parallel') (Join-Path $build 'consumer-build.log')
        $oldPath=$env:PATH
        try {
            $env:PATH="$prefix/bin;$oldPath"
            Invoke-Checked ctest @('--test-dir',$consumer,'--output-on-failure') (Join-Path $build 'consumer-ctest.log')
        } finally { $env:PATH=$oldPath }
        Write-Output "PASS $name : portable/native fault tests, exact symbols, enumeration and installed C/C++ consumers"
    }
}
