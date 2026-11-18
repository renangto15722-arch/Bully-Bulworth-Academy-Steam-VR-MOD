# dump-exe.ps1 - write the RUNNING Bully.exe image (SteamStub already
# decrypted it in memory) to a PE file Ghidra can load. Read-only on the game:
# it only reads its memory. Sections are written at their virtual addresses
# (raw offset = RVA), so file offsets in the dump equal RVAs.
#
#   .\tools\dump-exe.ps1 -Out K:\bullyvrpc_re\Bully_dump.exe
[CmdletBinding()]
param(
    [string]$Out = "K:\bullyvrpc_re\Bully_dump.exe",
    [string]$Exe = "K:\SteamLibrary\steamapps\common\Bully Scholarship Edition\Bully.exe"
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Mem {
    [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(int access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, int size, out int read);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
"@

$p = Get-Process Bully -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { throw "Bully is not running - start it through Steam and wait for the menu." }

# headers from the file on disk: image base, size, sections
$file = [IO.File]::ReadAllBytes($Exe)
$pe   = [BitConverter]::ToInt32($file, 0x3C)
$nsec = [BitConverter]::ToUInt16($file, $pe + 6)
$optSize = [BitConverter]::ToUInt16($file, $pe + 20)
$opt  = $pe + 24
$imageBase = [BitConverter]::ToUInt32($file, $opt + 28)
$sizeOfImage = [BitConverter]::ToUInt32($file, $opt + 56)
$dllChars = [BitConverter]::ToUInt16($file, $opt + 70)
if ($dllChars -band 0x40) { Write-Host "note: DYNAMIC_BASE set - using the main module's real base" }

$h = [Mem]::OpenProcess(0x0410, $false, $p.Id)   # QUERY_INFORMATION | VM_READ
if ($h -eq [IntPtr]::Zero) { throw "OpenProcess failed" }
try {
    $base = $imageBase
    try { $base = [uint32]$p.MainModule.BaseAddress.ToInt64() } catch { }
    $img = New-Object byte[] $sizeOfImage
    $page = New-Object byte[] 4096
    $bad = 0
    for ($off = 0; $off -lt $sizeOfImage; $off += 4096) {
        $read = 0
        if ([Mem]::ReadProcessMemory($h, [IntPtr]([int64]$base + $off), $page, 4096, [ref]$read) -and $read -eq 4096) {
            [Array]::Copy($page, 0, $img, $off, 4096)
        } else { $bad++ }
    }
} finally { [void][Mem]::CloseHandle($h) }

# raw layout = virtual layout
$secTable = $opt + $optSize
for ($i = 0; $i -lt $nsec; $i++) {
    $s = $secTable + $i * 40
    $vsize = [BitConverter]::ToUInt32($img, $s + 8)
    $va    = [BitConverter]::ToUInt32($img, $s + 12)
    [Array]::Copy([BitConverter]::GetBytes([uint32]$vsize), 0, $img, $s + 16, 4)   # SizeOfRawData
    [Array]::Copy([BitConverter]::GetBytes([uint32]$va),    0, $img, $s + 20, 4)   # PointerToRawData
    $name = [Text.Encoding]::ASCII.GetString($img, $s, 8).TrimEnd([char]0)
    Write-Host ("  {0,-8} RVA {1:x8} size {2:x8}" -f $name, $va, $vsize)
}
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
[IO.File]::WriteAllBytes($Out, $img)
Write-Host ("dumped {0} bytes from base {1:x8} ({2} unreadable pages) -> {3}" -f $sizeOfImage, $base, $bad, $Out)
