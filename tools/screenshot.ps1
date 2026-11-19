param(
    [string]$ProcessName = "WiiCompiled",
    [string]$OutFile = "build\screenshots\latest.png"
)
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32 {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
    public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
New-Item -ItemType Directory -Force (Split-Path $OutFile) | Out-Null
$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { Write-Output "window-not-found"; exit 1 }
$h = $proc.MainWindowHandle
# A minimised window captures as just its title bar: restore it (SW_RESTORE = 9) and let it repaint.
[Win32]::ShowWindow($h, 9) | Out-Null
Start-Sleep -Milliseconds 800
$rect = New-Object Win32+RECT
[Win32]::GetWindowRect($h, [ref]$rect) | Out-Null
$w = $rect.Right - $rect.Left; $ht = $rect.Bottom - $rect.Top
# Composited capture (works behind other windows and for GPU swapchains); flag 2 = PW_RENDERFULLCONTENT.
$b = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($b)
$hdc = $g.GetHdc()
$ok = [Win32]::PrintWindow($h, $hdc, 2)
$g.ReleaseHdc($hdc)
$g.Dispose()
$b.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
$b.Dispose()
Write-Output "saved $OutFile ($w x $ht) printwindow=$ok"
