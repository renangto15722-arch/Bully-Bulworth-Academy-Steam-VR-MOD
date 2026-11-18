# test-grabr2.ps1 -Left "<fwd> <side> <up>" - right-hand grab on the test dummy
# with the free left controller at the given offset (relative to the head);
# then releases. Prints whether Jimmy reached Hold_Idle.
param([string]$Left = "0.25 -0.12 -0.35", [switch]$Place)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$cmd  = Join-Path $PSScriptRoot 'xrsim-cmd.ps1'
$dir  = "$env:LOCALAPPDATA\BullyVR\xrsim"
$log  = 'K:\SteamLibrary\steamapps\common\Bully Scholarship Edition\BullyVR.log'
function Cmd([string[]]$lines) { & $cmd -Dir $dir @lines | Out-Null }
if ($Place) {
    & (Join-Path $root 'tools\bully-key.ps1') -Key Home; Start-Sleep 1
    & (Join-Path $root 'tools\bully-key.ps1') -Key Home; Start-Sleep 4
}
$n0 = (Get-Content $log).Count
Cmd @("grip l 0", "grip r 0", "head rot 0 0 0", "hand l offset $Left", "hand r offset 0.62 0.12 -0.18")
Start-Sleep -Milliseconds 1500
Cmd @("grip r 1")
Start-Sleep -Milliseconds 2500
$new = Get-Content $log | Select-Object -Skip $n0
$new | Select-String "wrestle\] (grab|NOT|EXEC)|act\] JIMMY" | ForEach-Object { $_.Line.Substring(0, [Math]::Min(140, $_.Line.Length)) }
Cmd @("grip r 0"); Start-Sleep -Milliseconds 400
Cmd @("grip r 1"); Start-Sleep -Milliseconds 400
Cmd @("grip r 0"); Start-Sleep -Milliseconds 1500
