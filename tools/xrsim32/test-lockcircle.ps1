# test-lockcircle.ps1 - lock on to the test dummy (left trigger), both arms
# held out in front, and strafe around it with the left stick for -Seconds.
# Screenshots go to tmp\lock_*.png. Assumes the dummy is placed (Home).
param([double]$Seconds = 6, [double]$Stick = 1.0)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$cmd  = Join-Path $PSScriptRoot 'xrsim-cmd.ps1'
$shot = Join-Path $PSScriptRoot 'xrsim-shot.ps1'
$dir  = "$env:LOCALAPPDATA\BullyVR\xrsim"
& $cmd -Dir $dir "hand l offset 0.45 -0.15 -0.25" "hand r offset 0.45 0.15 -0.25" "trigger l 1" | Out-Null
Start-Sleep -Milliseconds 800
& $shot -Dir $dir -Out (Join-Path $root 'tmp\lock_0') | Out-Null
& $cmd -Dir $dir "stick l $Stick 0" | Out-Null
$t0 = Get-Date; $i = 1
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    Start-Sleep -Milliseconds 1500
    & $shot -Dir $dir -Out (Join-Path $root "tmp\lock_$i") | Out-Null
    $i++
}
& $cmd -Dir $dir "stick l 0 0" | Out-Null
Start-Sleep -Milliseconds 800
& $cmd -Dir $dir "trigger l 0" | Out-Null
Start-Sleep -Milliseconds 1200
& $shot -Dir $dir -Out (Join-Path $root 'tmp\lock_end') | Out-Null
"done"
