# bully-uninstall.ps1 - the game folder back to the original game.
# Keeps BullyVR.ini (the options) unless -All; bullyvr\ only goes with -All.
[CmdletBinding()]
param(
    [switch]$All,
    [string]$Game = "K:\SteamLibrary\steamapps\common\Bully Scholarship Edition"
)
$ErrorActionPreference = 'Stop'
if (Get-Process Bully -ErrorAction SilentlyContinue) { throw "Bully is running - close it first." }
Remove-Item "$Game\d3d9.dll", "$Game\BullyVR.log", "$Game\Bully_d3d9.log" -ErrorAction SilentlyContinue
if ($All) {
    Remove-Item "$Game\BullyVR.ini" -ErrorAction SilentlyContinue
    Remove-Item "$Game\bullyvr" -Recurse -ErrorAction SilentlyContinue
}
Write-Host "mod removed from $Game"
