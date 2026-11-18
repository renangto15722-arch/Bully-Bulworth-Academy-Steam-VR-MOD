# bully-key.ps1 -Key F9 [-Key F11 ...] - bring the Bully window to the front and
# tap keys (the mod's live test switches: F8 recentre, F9 hide head, F10 arm IK,
# F11 game camera follows the headset).
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string[]]$Key)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class BvrKey {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
    public static void Tap(byte vk) {
        byte sc = (byte)MapVirtualKey(vk, 0);
        keybd_event(vk, sc, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(80);
        keybd_event(vk, sc, 2, UIntPtr.Zero);
    }
}
"@
$p = Get-Process Bully -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
[BvrKey]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 200
foreach ($k in $Key) {
    $vk = [byte][System.Windows.Forms.Keys]::Parse([System.Windows.Forms.Keys], $k)
    [BvrKey]::Tap($vk)
    Start-Sleep -Milliseconds 300
}
