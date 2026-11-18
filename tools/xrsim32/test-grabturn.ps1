# test-grabturn.ps1 -Hand r|l - grab with that hand, then sweep its controller
# 40 deg to the left and to the right (around the head); screenshots looking
# down at each: tmp\gt<hand>_<step>.png
param([ValidateSet('l', 'r')][string]$Hand = 'r')
$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$cmd   = Join-Path $PSScriptRoot 'xrsim-cmd.ps1'
$shotT = Join-Path $PSScriptRoot 'xrsim-shot.ps1'
$dir   = "$env:LOCALAPPDATA\BullyVR\xrsim"
$free  = if ($Hand -eq 'l') { 'r' } else { 'l' }
$side  = if ($Hand -eq 'l') { -0.12 } else { 0.12 }
function Cmd([string[]]$lines) { & $cmd -Dir $dir @lines | Out-Null }
function Shot([string]$n) { & $shotT -Dir $dir -Out (Join-Path $root "tmp\gt$($Hand)_$n") | Out-Null }
Cmd @("head rot 0 0 0", "hand $free offset 0.25 $(-$side) -0.35", "hand $Hand offset 0.62 $side -0.18")
Start-Sleep -Milliseconds 800
Cmd @("grip $Hand 1"); Start-Sleep -Milliseconds 2000
Shot '0start'
# sweep the holding controller round the head (radius ~0.63 m, at -0.18 up)
foreach ($a in @(10, 20, 30, 40)) {
    $r = [Math]::PI * $a / 180
    Cmd @("hand $Hand offset $([Math]::Round(0.63*[Math]::Cos($r),3)) $([Math]::Round(-0.63*[Math]::Sin($r),3)) -0.18")
    Start-Sleep -Milliseconds 300
}
Start-Sleep -Milliseconds 1200
Shot '1left'
foreach ($a in @(20, 0, -20, -40)) {
    $r = [Math]::PI * $a / 180
    Cmd @("hand $Hand offset $([Math]::Round(0.63*[Math]::Cos($r),3)) $([Math]::Round(-0.63*[Math]::Sin($r),3)) -0.18")
    Start-Sleep -Milliseconds 300
}
Start-Sleep -Milliseconds 1200
Shot '2right'
Cmd @("grip $Hand 0"); Start-Sleep -Milliseconds 400
Cmd @("grip $Hand 1"); Start-Sleep -Milliseconds 400
Cmd @("grip $Hand 0"); Start-Sleep -Milliseconds 1500
"done"
