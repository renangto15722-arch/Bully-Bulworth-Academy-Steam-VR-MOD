# test-grab1.ps1 -Hand l|r - one-hand grab on the test dummy (Home) with the
# simulated controller, then: turn the head (the body follows it), lean,
# punch with the free hand, look down at the holding arm, let go.
# Screenshots: tmp\g<hand>_<step>.png
param([ValidateSet('l', 'r')][string]$Hand = 'l')
$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$cmd   = Join-Path $PSScriptRoot 'xrsim-cmd.ps1'
$shotT = Join-Path $PSScriptRoot 'xrsim-shot.ps1'
$dir   = "$env:LOCALAPPDATA\BullyVR\xrsim"
$free  = if ($Hand -eq 'l') { 'r' } else { 'l' }
$side  = if ($Hand -eq 'l') { -0.12 } else { 0.12 }
function Cmd([string[]]$lines) { & $cmd -Dir $dir @lines | Out-Null }
function Shot([string]$n) { & $shotT -Dir $dir -Out (Join-Path $root "tmp\g$($Hand)_$n") | Out-Null }

Cmd @("head rot 0 0 0", "hand $free offset 0.25 $(-$side) -0.35", "hand $Hand offset 0.62 $side -0.18")
Start-Sleep -Milliseconds 700
Cmd "grip $Hand 1"
Start-Sleep -Milliseconds 2500
Shot '1hold'
Cmd "head rot 0 -35 0"
Start-Sleep -Milliseconds 800
Shot '2down'
Cmd "head rot 30 -35 0"
Start-Sleep -Milliseconds 1500
Shot '3turn'
Cmd "head rot 0 0 0"
Start-Sleep -Milliseconds 1000
Shot '3level'
python (Join-Path $PSScriptRoot 'xrsim-punch.py') $free 6 0.75 | Out-Null
Start-Sleep -Milliseconds 150
Shot '4punch'
Start-Sleep -Milliseconds 1200
Shot '5after'
Cmd "head rot 0 -35 0"
Start-Sleep -Milliseconds 600
Shot '6afterdown'
Cmd "head rot 0 0 0"
Cmd "grip $Hand 0"
Start-Sleep -Milliseconds 500
Cmd "grip $Hand 1"          # holding: press + release without pushing = let go
Start-Sleep -Milliseconds 500
Cmd "grip $Hand 0"
Start-Sleep -Milliseconds 1500
Cmd "head rot 0 0 0"
"done"
