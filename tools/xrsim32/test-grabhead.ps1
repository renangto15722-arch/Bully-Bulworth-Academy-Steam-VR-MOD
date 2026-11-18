# test-grabhead.ps1 -Hand r|l - grab with that hand, then pin that controller
# where it is (room space) and move / turn ONLY the head: the held kid must
# not move. Then sweep the controller to the left: the kid must follow.
# Prints the [grabturn] lines; screenshots tmp\gh<hand>_<step>.png
param([ValidateSet('l', 'r')][string]$Hand = 'r')
$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$cmd   = Join-Path $PSScriptRoot 'xrsim-cmd.ps1'
$shotT = Join-Path $PSScriptRoot 'xrsim-shot.ps1'
$dir   = "$env:LOCALAPPDATA\BullyVR\xrsim"
$log   = 'K:\SteamLibrary\steamapps\common\Bully Scholarship Edition\BullyVR.log'
$free  = if ($Hand -eq 'l') { 'r' } else { 'l' }
$side  = if ($Hand -eq 'l') { -0.12 } else { 0.12 }
function Cmd([string[]]$lines) { & $cmd -Dir $dir @lines | Out-Null }
function F([double]$v) { $v.ToString('0.0000', [Globalization.CultureInfo]::InvariantCulture) }
function Shot([string]$n) { & $shotT -Dir $dir -Out (Join-Path $root "tmp\gh$($Hand)_$n") | Out-Null }
$n0 = (Get-Content $log).Count
Cmd @("head rot 0 0 0", "hand $free offset 0.25 $(-$side) -0.35", "hand $Hand offset 0.62 $side -0.18")
Start-Sleep -Milliseconds 800
Cmd @("grip $Hand 1"); Start-Sleep -Milliseconds 2000
$st = Get-Content (Join-Path $dir 'state.json') -Raw | ConvertFrom-Json
$hp = if ($Hand -eq 'r') { $st.handR.pos } else { $st.handL.pos }
$hd = $st.head.pos
"head at $($hd -join ' '), hand at $($hp -join ' ')"
Cmd @("hand $Hand grip pos $(F $hp[0]) $(F $hp[1]) $(F $hp[2])")   # pinned in the room
Start-Sleep -Milliseconds 800
Shot '0pinned'
Cmd @("head pos $(F ($hd[0] + 0.25)) $(F ($hd[1] - 0.10)) $(F ($hd[2] + 0.10))", "head rot 35 0 0")
Start-Sleep -Milliseconds 2200
Shot '1headmoved'
Cmd @("head pos $(F $hd[0]) $(F $hd[1]) $(F $hd[2])", "head rot 0 0 0")
Start-Sleep -Milliseconds 1500
# the controller round to the left (room space: -x)
Cmd @("hand $Hand grip pos $(F ($hp[0] - 0.45)) $(F $hp[1]) $(F ($hp[2] + 0.15))")
Start-Sleep -Milliseconds 2200
Shot '2ctrlleft'
Cmd @("grip $Hand 0"); Start-Sleep -Milliseconds 400
Cmd @("grip $Hand 1"); Start-Sleep -Milliseconds 400
Cmd @("grip $Hand 0"); Start-Sleep -Milliseconds 1500
Get-Content $log | Select-Object -Skip $n0 | Select-String "grabturn|wrestle\] (grab|NOT|EXEC|grip)|act\] JIMMY.*(Hold_Idle/|GrappleSuccess)" |
    ForEach-Object { $_.Line.Substring(0, [Math]::Min(150, $_.Line.Length)) }
