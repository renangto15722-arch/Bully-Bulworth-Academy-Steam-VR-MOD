# bully-autostart.ps1 - walk the game from the intro into gameplay for a test run.
#
# Alt skips the intro movies and picks the menu entry that continues the save
# (what Renan presses by hand). The script brings the Bully window to the front,
# taps Alt every -Every seconds and stops as soon as BullyVR.log shows the
# stereo gameplay ("[stereo] recentred") or after -TimeoutSec.
# Nothing is saved or changed: it only continues the existing save.
#
#   .\tools\bully-autostart.ps1                 # after start steam://rungameid/12200
[CmdletBinding()]
param(
    [string]$Game = "K:\SteamLibrary\steamapps\common\Bully Scholarship Edition",
    [double]$Every = 2,
    [int]$TimeoutSec = 300
)
$ErrorActionPreference = 'Stop'

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class BvrKeys {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public static void Tap(byte vk, byte scan) {
        keybd_event(vk, scan, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(80);
        keybd_event(vk, scan, 2, UIntPtr.Zero);
    }
}
"@

$log   = Join-Path $Game "BullyVR.log"
$start = Get-Date
$taps  = 0
while (((Get-Date) - $start).TotalSeconds -lt $TimeoutSec) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern "\[stereo\] recentred" -Quiet)) {
        Write-Host "gameplay reached after $([int]((Get-Date) - $start).TotalSeconds) s ($taps Alt taps)"
        return
    }
    $p = Get-Process Bully -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($p) {
        [BvrKeys]::ShowWindow($p.MainWindowHandle, 9) | Out-Null   # SW_RESTORE
        [BvrKeys]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
        Start-Sleep -Milliseconds 150
        [BvrKeys]::Tap(0x12, 0x38)                                # VK_MENU (Alt), scan code 0x38
        $taps++
    }
    Start-Sleep -Seconds $Every
}
throw "no gameplay within $TimeoutSec s ($taps Alt taps) - look at the game window"
