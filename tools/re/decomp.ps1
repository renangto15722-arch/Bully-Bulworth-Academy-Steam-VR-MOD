# decomp.ps1 -Out file.c 0x4f37b0 0x8824f0 ... - decompile functions of the
# Bully dump (Ghidra project K:\bullyvrpc_re\ghidra\BullyPC) into one file.
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Names
)
$ErrorActionPreference = 'Stop'
$list = Join-Path $env:TEMP "bvr_decomp_list.txt"
Set-Content $list $Names -Encoding ASCII
& "C:\Tools\ghidra_12.0.4_PUBLIC\support\analyzeHeadless.bat" K:\bullyvrpc_re\ghidra BullyPC -process Bully_dump.exe `
    -scriptPath $PSScriptRoot -postScript DecompileList.java $list $Out 2>&1 |
    Select-String -Pattern "ERROR|Exception" | Select-Object -First 10
Write-Host "-> $Out"
