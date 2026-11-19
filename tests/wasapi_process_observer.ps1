# Device-free regression of the actual matrix process observer. Extract its
# function without evaluating the script's fixture validation or audio trials.
$ErrorActionPreference='Stop'
# CTest can inherit PowerShell 7's module path while invoking Windows PowerShell.
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility') -Force
$tokens=$null; $errors=$null
$path=Join-Path $PSScriptRoot '../tools/run_wasapi_marker_matrix.ps1'
$ast=[Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Matrix script parse failed'}
$function=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Invoke-Observed'},$true)
if(!$function){throw 'Actual observer function not found'}
. ([scriptblock]::Create($function.Extent.Text))
$output=Join-Path ([IO.Path]::GetTempPath()) ('hail-observer-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output | Out-Null
try {
    foreach($code in 0,7) {
        $r=Invoke-Observed "$env:SystemRoot/System32/cmd.exe" @('/d','/c','exit',"$code") "exit-$code" 10
        if($null -eq $r.exit_code -or $r.exit_code -ne $code -or $r.watchdog_expired){throw "Lost exit code $code"}
    }
    $r=Invoke-Observed "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe" @('-NoProfile','-NonInteractive','-Command','Start-Sleep 5') 'timeout' 0
    if(!$r.watchdog_expired -or $null -eq $r.exit_code -or $r.exit_code -eq 0){throw 'Watchdog must record failure'}
    Write-Output 'Actual redirected observer preserves success/failure exit codes and watchdog failure'
} finally {
    Get-ChildItem -LiteralPath $output -File | ForEach-Object {Remove-Item -LiteralPath $_.FullName}
    Remove-Item -LiteralPath $output
}
