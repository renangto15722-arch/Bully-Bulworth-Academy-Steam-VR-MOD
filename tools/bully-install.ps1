# bully-install.ps1 - put the built mod into the game folder.
#
#   .\tools\bully-install.ps1          # real headset (the machine's OpenXR runtime)
#   .\tools\bully-install.ps1 -Sim     # the 32-bit OpenXR simulator (tools\xrsim32)
#
# Bully only starts through Steam, so the simulator cannot be picked with an
# environment variable: -Sim writes BullyVR.ini [VR] RuntimeJson / SimDir and
# the mod sets them inside the game process. Without -Sim those two keys are
# removed and every other option in BullyVR.ini is kept.
# Refuses while Bully is running (it would hold the files).
[CmdletBinding()]
param(
    [switch]$Sim,
    [string]$Game  = "K:\SteamLibrary\steamapps\common\Bully Scholarship Edition",
    [string]$Build = "K:\bullyvrpc_build\RelWithDebInfo",
    [string]$Dxvk  = "K:\bullyvrpc_deps\dxvk-3.1.1\dxvk-3.1.1\x32\d3d9.dll",
    [string]$SimDir = "$env:LOCALAPPDATA\BullyVR\xrsim"
)
$ErrorActionPreference = 'Stop'

if (Get-Process Bully -ErrorAction SilentlyContinue) { throw "Bully is running - close it first." }
if (-not (Test-Path "$Game\Bully.exe")) { throw "Bully.exe not found in $Game" }

New-Item -ItemType Directory -Force "$Game\bullyvr" | Out-Null
Copy-Item "$Build\d3d9.dll" "$Game\d3d9.dll" -Force
Copy-Item $Dxvk "$Game\bullyvr\dxvk_d3d9.dll" -Force

# --- BullyVR.ini: only the simulator keys are touched ------------------------
$ini   = "$Game\BullyVR.ini"
$lines = if (Test-Path $ini) { @(Get-Content $ini) } else { @() }
$lines = @($lines | Where-Object { $_ -notmatch '^\s*(RuntimeJson|SimDir)\s*=' })
if ($Sim) {
    $dll = Join-Path $PSScriptRoot "xrsim32\bvr_xrsim32.dll"
    if (-not (Test-Path $dll)) { throw "simulator dll missing: $dll" }
    New-Item -ItemType Directory -Force $SimDir, "$SimDir\capture" | Out-Null
    foreach ($leaf in "command.txt", "state.json", "ack.txt") { Remove-Item "$SimDir\$leaf" -ErrorAction SilentlyContinue }
    $manifest = Join-Path $SimDir "bvr_xrsim32.json"
    $json = '{"file_format_version":"1.0.0","runtime":{"name":"bvr-xrsim","library_path":"' + $dll.Replace('\', '\\') + '"}}'
    [IO.File]::WriteAllText($manifest, $json, [Text.UTF8Encoding]::new($false))   # no BOM: the loader rejects one

    $i = [Array]::FindIndex([string[]]$lines, [Predicate[string]]{ param($l) $l -match '^\s*\[VR\]\s*$' })
    $keys = @("RuntimeJson=$manifest", "SimDir=$SimDir")
    if ($i -lt 0) { $lines += "[VR]"; $lines += $keys }
    else { $lines = @($lines[0..$i]) + $keys + @(if ($i + 1 -lt $lines.Count) { $lines[($i + 1)..($lines.Count - 1)] }) }
}
if ($lines.Count -gt 0) { Set-Content $ini $lines -Encoding ASCII } elseif (Test-Path $ini) { Remove-Item $ini }

# the logs stay: BullyVR.log is moved to BullyVR.prev.log by the next run
Write-Host ("installed {0} ({1})" -f (Get-Item "$Game\d3d9.dll").LastWriteTime, $(if ($Sim) { "SIMULATOR: $SimDir" } else { "real runtime" }))
