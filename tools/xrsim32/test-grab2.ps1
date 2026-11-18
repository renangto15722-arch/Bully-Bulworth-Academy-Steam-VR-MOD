# test-grab2.ps1 - both-hands grab on the test dummy (Home): both controllers
# on the kid, both grips. Then the right controller moved away (the right arm
# must follow it - free from the start), a right punch, and let go (left grip
# pressed + released). Screenshots tmp\g2_<step>.png; prints the log lines.
param([switch]$RightAway)
$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$cmd   = Join-Path $PSScriptRoot 'xrsim-cmd.ps1'
$shotT = Join-Path $PSScriptRoot 'xrsim-shot.ps1'
$dir   = "$env:LOCALAPPDATA\BullyVR\xrsim"
$log   = 'K:\SteamLibrary\steamapps\common\Bully Scholarship Edition\BullyVR.log'
function Cmd([string[]]$lines) { & $cmd -Dir $dir @lines | Out-Null }
function Shot([string]$n) { & $shotT -Dir $dir -Out (Join-Path $root "tmp\g2_$n") | Out-Null }
$n0 = (Get-Content $log).Count
Cmd @("grip l 0", "grip r 0", "head rot 0 0 0", "hand l offset 0.62 -0.12 -0.18", "hand r offset 0.62 0.12 -0.18")
Start-Sleep -Milliseconds 1200
Cmd @("grip l 1", "grip r 1")
if ($RightAway) { Start-Sleep -Milliseconds 60; Cmd @("hand r offset 0.25 0.20 -0.35") }
Start-Sleep -Milliseconds 150
Shot '0landing'
Start-Sleep -Milliseconds 2000
Shot '1hold'
Cmd @("grip r 0", "hand r offset 0.30 0.35 -0.05")   # the right hand away, up to the right
Start-Sleep -Milliseconds 1200
Shot '2rightaway'
python (Join-Path $PSScriptRoot 'xrsim-punch.py') r 6 0.75 | Out-Null
Start-Sleep -Milliseconds 150
Shot '3punch'
Start-Sleep -Milliseconds 1500
Shot '4after'
Cmd @("grip l 0"); Start-Sleep -Milliseconds 400
Cmd @("grip l 1"); Start-Sleep -Milliseconds 400
Cmd @("grip l 0"); Start-Sleep -Milliseconds 1500
Get-Content $log | Select-Object -Skip $n0 | Select-String "wrestle\] (grab|NOT|EXEC|grip|.*fist punches)|act\] JIMMY.*(Hold_Idle/|GrappleSuccess)|\[body\] (left|right|both)" |
    ForEach-Object { $_.Line.Substring(0, [Math]::Min(150, $_.Line.Length)) }
