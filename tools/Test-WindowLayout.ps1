param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\out\build\vs2026-x64\Release\WiFiStudio.exe'),
    [string]$Screenshot = (Join-Path $PSScriptRoot '..\out\artifacts\gui-compact.png'),
    [ValidateSet('auto', 'ru', 'en')]
    [string]$Language = 'auto'
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class LayoutTest {
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
 [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr w);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr w,IntPtr a,int x,int y,int width,int height,uint flags);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr w,out Rect r);
 [DllImport("user32.dll")] public static extern bool GetScrollInfo(IntPtr w,int bar,ref ScrollInfo s);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr w,IntPtr dc,uint flags);
 [DllImport("user32.dll",EntryPoint="SendMessageW")] public static extern IntPtr Send(IntPtr w,uint m,IntPtr wp,IntPtr lp);
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
 [StructLayout(LayoutKind.Sequential)] public struct ScrollInfo { public uint Size,Mask; public int Min,Max; public uint Page; public int Pos,Track; }
}
'@
if (Get-Process WiFiStudio -ErrorAction SilentlyContinue) { throw 'Close WiFi Studio before this test.' }
[void][LayoutTest]::SetThreadDpiAwarenessContext([IntPtr](-4))
$path = [IO.Path]::GetFullPath($Executable)
$arguments = @('--no-activate')
if ($Language -ne 'auto') { $arguments += @('--lang', $Language) }
# Regression: closing while adapter discovery may still be in flight must not show a modal dialog.
$p = Start-Process $path -ArgumentList $arguments -PassThru
[void]$p.WaitForInputIdle(10000)
[void]$p.CloseMainWindow()
if (-not $p.WaitForExit(30000)) { throw 'Close during initialization did not finish automatically.' }
Write-Output 'PASS: close during initialization completes without a waiting dialog.'
$p = Start-Process $path -ArgumentList $arguments -PassThru
try {
    [void]$p.WaitForInputIdle(10000)
    $p.Refresh(); $window = $p.MainWindowHandle
    if ($window -eq [IntPtr]::Zero) { throw 'Main window not available.' }
    $dpi = [LayoutTest]::GetDpiForWindow($window)
    [void][LayoutTest]::SetWindowPos($window, [IntPtr]::Zero, 0, 0, [int](960*$dpi/96), [int](660*$dpi/96), 0x16)
    $scroll = [LayoutTest+ScrollInfo]::new()
    $scroll.Size = [Runtime.InteropServices.Marshal]::SizeOf($scroll); $scroll.Mask = 0x17
    if (-not [LayoutTest]::GetScrollInfo($window, 1, [ref]$scroll) -or $scroll.Max -lt $scroll.Page) {
        throw 'Compact window has no scroll range for the offscreen content.'
    }
    [void][LayoutTest]::Send($window, 0x115, [IntPtr]7, [IntPtr]::Zero) # WM_VSCROLL / SB_BOTTOM
    $scroll.Mask=4
    [void][LayoutTest]::GetScrollInfo($window,1,[ref]$scroll)
    if($scroll.Pos -le 0){ throw 'Compact window did not scroll.' }
    $rect=[LayoutTest+Rect]::new(); [void][LayoutTest]::GetWindowRect($window,[ref]$rect)
    $bitmap=[Drawing.Bitmap]::new($rect.Right-$rect.Left,$rect.Bottom-$rect.Top)
    $graphics=[Drawing.Graphics]::FromImage($bitmap); $dc=$graphics.GetHdc()
    try { if(-not [LayoutTest]::PrintWindow($window,$dc,2)){throw 'PrintWindow failed'} } finally {$graphics.ReleaseHdc($dc)}
    $fullPath=[IO.Path]::GetFullPath($Screenshot)
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($fullPath))
    $bitmap.Save($fullPath,[Drawing.Imaging.ImageFormat]::Png); $graphics.Dispose();$bitmap.Dispose()
    Write-Output "PASS: 960x660 logical window, DPI $dpi, vertical scrolling; screenshot $fullPath"
} finally {
    if(-not $p.HasExited){[void]$p.CloseMainWindow(); if(-not $p.WaitForExit(30000)){Write-Warning 'GUI did not finish graceful close.'}}
}
