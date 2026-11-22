param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\out\build\vs2026-x64\Release\WiFiStudio.exe'),
    [string]$Screenshot = (Join-Path $PSScriptRoot '..\out\artifacts\gui.png'),
    [switch]$HardwareCycle,
    [ValidateSet('wpa2', 'transition', 'wpa3')]
    [string]$Security = 'wpa2',
    [ValidateSet('auto', 'ru', 'en')]
    [string]$Language = 'auto',
    [switch]$LeaveOpen
)
# This drives the real native GUI. HardwareCycle is explicitly opt-in.
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class GuiNative {
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll", EntryPoint="GetWindowLongW")] public static extern int GetWindowStyle(IntPtr window, int index);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string className, IntPtr title);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageText(IntPtr hwnd, uint msg, IntPtr wp, StringBuilder text);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SetControlText(IntPtr hwnd, uint msg, IntPtr wp, string text);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out Rect rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr dc, uint flags);
    [DllImport("kernel32.dll")] public static extern ushort GetUserDefaultUILanguage();
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
    private delegate bool EnumWindowProc(IntPtr hwnd, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumThreadWindows(uint thread, EnumWindowProc callback, IntPtr parameter);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetClassName(IntPtr hwnd, StringBuilder name, int count);
    public static IntPtr FindDialog(IntPtr parent) {
        uint processId;
        uint thread = GetWindowThreadProcessId(parent, out processId);
        IntPtr result = IntPtr.Zero;
        EnumThreadWindows(thread, (hwnd, parameter) => {
            var name = new StringBuilder(64);
            GetClassName(hwnd, name, name.Capacity);
            if (name.ToString() == "#32770") { result = hwnd; return false; }
            return true;
        }, IntPtr.Zero);
        return result;
    }
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
}
'@
function Get-ControlText([int]$Id) {
    $text = [System.Text.StringBuilder]::new(64000)
    [void][GuiNative]::SendMessageText([GuiNative]::GetDlgItem($script:window, $Id), 0xD, [IntPtr]$text.Capacity, $text)
    $text.ToString()
}
function Get-DialogText([IntPtr]$Dialog) {
    # TaskDialog uses DirectUI, not native static controls. Read only this dialog;
    # never inspect the main window's password editor through UI Automation.
    $element = [System.Windows.Automation.AutomationElement]::FromHandle($Dialog)
    $children = $element.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)
    (@($element.Current.Name) + @($children | ForEach-Object { $_.Current.Name })) -join "`n"
}
function Click-Control([int]$Id) {
    $handle = [GuiNative]::GetDlgItem($script:window, $Id)
    if (-not [GuiNative]::IsWindowEnabled($handle)) { throw "Control $Id is disabled" }
    # Post rather than Send: an error TaskDialog must not deadlock the test process.
    [void][GuiNative]::PostMessage($script:window, 0x111, [IntPtr]$Id, $handle)
}
function Wait-Until([scriptblock]$Condition, [string]$Description, [int]$Timeout = 30) {
    $end = [DateTime]::UtcNow.AddSeconds($Timeout)
    do {
        if (& $Condition) { return }
        if ($script:process.HasExited) { throw "GUI exited (code $($script:process.ExitCode)) while waiting for $Description. Log: $(Get-ControlText 1013)" }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $end)
    throw "Timeout waiting for $Description. Log: $(Get-ControlText 1013)"
}
[void][GuiNative]::SetThreadDpiAwarenessContext([IntPtr](-4)) # Per-monitor V2: capture physical pixels.
$path = [System.IO.Path]::GetFullPath($Executable)
$existing = Get-Process WiFiStudio -ErrorAction SilentlyContinue
if ($existing) { throw 'Close the existing WiFi Studio window before running this test.' }
$expectedLanguage = if ($Language -eq 'auto') {
    if (([GuiNative]::GetUserDefaultUILanguage() -band 0x3ff) -eq 0x19) { 'ru' } else { 'en' }
} else { $Language }
$strings = if ($expectedLanguage -eq 'ru') {
    @{ Discovery = 'Найдено Wi-Fi-адаптеров'; Off = 'Выключена'; On = 'В эфире';
       Title = 'WiFi Studio — точка доступа Windows 11'; Show = 'Показать'; New = 'Новый';
       Refresh = 'Обновить'; Start = 'Включить точку доступа'; Settings = 'Параметры хот-спота Windows';
       Band = 'Автоматически'; ErrorHeading = 'Операция не выполнена'; SsidError = 'Имя сети:'; Ok = 'ОК' }
} else {
    @{ Discovery = 'Wi-Fi adapters found:'; Off = 'Off'; On = 'On air';
       Title = 'WiFi Studio — Windows 11 hotspot'; Show = 'Show'; New = 'New';
       Refresh = 'Refresh'; Start = 'Start hotspot'; Settings = 'Windows hotspot settings';
       Band = 'Automatic'; ErrorHeading = 'Operation failed'; SsidError = 'Network name:'; Ok = 'OK' }
}
$discoveryPattern = [regex]::Escape($strings.Discovery)
$arguments = @('--no-activate')
if ($Language -ne 'auto') { $arguments += @('--lang', $Language) }
$script:process = Start-Process -FilePath $path -ArgumentList $arguments -PassThru
$script:window = [IntPtr]::Zero
try {
    Wait-Until { $script:process.Refresh(); $script:window = [GuiNative]::FindWindow('WiFiStudio.MainWindow', [IntPtr]::Zero); $script:window -ne [IntPtr]::Zero } 'main window'
    Wait-Until { (Get-ControlText 1013) -match $discoveryPattern } 'adapter discovery'
    $passwordHandle = [GuiNative]::GetDlgItem($window, 1004)
    if ($passwordHandle -eq [IntPtr]::Zero) { throw 'Password editor was not created.' }
    $style = [GuiNative]::GetWindowStyle($passwordHandle, -16)
    $mask = [GuiNative]::SendMessage($passwordHandle, 0xD2, [IntPtr]::Zero, [IntPtr]::Zero)
    if (($style -band 0x20) -eq 0 -or $mask -eq [IntPtr]::Zero) { throw 'Native ES_PASSWORD protection / masking is missing.' }
    # This OS build's stock UIA proxy reports IsPassword=False even for ES_PASSWORD
    # and exposes no ValuePattern. Check the native contract instead; never read the password.
    $count = [GuiNative]::SendMessage([GuiNative]::GetDlgItem($window, 1001), 0x146, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()
    if ($count -lt 1) { throw 'Adapter combo is empty.' }
    Click-Control 1009 # Generate a new password, without reading the password into this test.
    $discoveries = ([regex]::Matches((Get-ControlText 1013), $discoveryPattern)).Count
    Click-Control 1010 # Refresh adapters (posted asynchronously).
    Wait-Until {
        [GuiNative]::IsWindowEnabled([GuiNative]::GetDlgItem($window, 1010)) -and
        ([regex]::Matches((Get-ControlText 1013), $discoveryPattern)).Count -gt $discoveries
    } 'refresh completion'
    $title = [Text.StringBuilder]::new(256)
    [void][GuiNative]::GetWindowText($window, $title, $title.Capacity)
    if ($title.ToString() -ne $strings.Title) { throw "Window title does not match $expectedLanguage." }
    foreach ($expected in @{
        1008 = $strings.Show; 1009 = $strings.New; 1010 = $strings.Refresh;
        1011 = $strings.Start; 1012 = $strings.Settings; 1005 = $strings.Band
    }.GetEnumerator()) {
        if ((Get-ControlText $expected.Key) -ne $expected.Value) {
            throw "Control $($expected.Key) does not match $expectedLanguage."
        }
    }
    Write-Output "PASS: $expectedLanguage window title, buttons, band and discovery log (requested: $Language)."
    if ((Get-ControlText 1015) -eq $strings.Off -and [GuiNative]::IsWindowEnabled([GuiNative]::GetDlgItem($window, 1011))) {
        # Empty SSID must fail validation before any network operation is submitted.
        $ssid = Get-ControlText 1003
        $dialog = [IntPtr]::Zero
        try {
            [void][GuiNative]::SetControlText([GuiNative]::GetDlgItem($window, 1003), 0xC, [IntPtr]::Zero, '')
            Click-Control 1011
            Wait-Until { $script:dialog = [GuiNative]::FindDialog($window); $script:dialog -ne [IntPtr]::Zero } 'validation dialog'
            Wait-Until {
                $script:dialogText = Get-DialogText $dialog
                $script:dialogText.Contains($strings.ErrorHeading) -and $script:dialogText.Contains($strings.SsidError) -and
                    $script:dialogText.Contains($strings.Ok)
            } 'translated validation dialog and OK caption' 10
            Write-Output "PASS: $expectedLanguage validation dialog and OK caption; no network operation."
        } finally {
            if ($dialog -ne [IntPtr]::Zero -and [GuiNative]::IsWindow($dialog)) {
                [void][GuiNative]::PostMessage($dialog, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
                Wait-Until { -not [GuiNative]::IsWindow($dialog) } 'validation dialog close'
            }
            [void][GuiNative]::SetControlText([GuiNative]::GetDlgItem($window, 1003), 0xC, [IntPtr]::Zero, $ssid)
        }
    }
    Write-Output 'PASS: native window, adapter discovery, password masking, generation, refresh.'
    if ($HardwareCycle) {
        if ((Get-ControlText 1015) -ne $strings.Off) { throw 'Hardware test refused: initial state must be Off.' }
        [void][GuiNative]::SetControlText([GuiNative]::GetDlgItem($window, 1003), 0xC, [IntPtr]::Zero, 'WiFiStudio-GuiTest')
        if ((Get-ControlText 1003) -ne 'WiFiStudio-GuiTest') { throw 'Custom SSID was not entered.' }
        # 2.4 GHz; no change to the main connection performance priority.
        $securityIndex = switch ($Security) {
            'wpa2' { 0 }
            'transition' { 1 }
            'wpa3' { 2 }
        }
        $bandHandle = [GuiNative]::GetDlgItem($window, 1005)
        $securityHandle = [GuiNative]::GetDlgItem($window, 1006)
        [void][GuiNative]::SendMessage($bandHandle, 0x14E, [IntPtr]1, [IntPtr]::Zero)
        [void][GuiNative]::SendMessage($securityHandle, 0x14E, [IntPtr]$securityIndex, [IntPtr]::Zero)
        if ([GuiNative]::SendMessage($bandHandle, 0x147, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32() -ne 1) {
            throw 'The 2.4 GHz band was not selected.'
        }
        if ([GuiNative]::SendMessage($securityHandle, 0x147, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32() -ne $securityIndex) {
            throw "The requested security mode $Security was not selected."
        }
        Click-Control 1011
        Wait-Until { (Get-ControlText 1015) -eq $strings.On } 'hotspot On' 60
        Write-Output "PASS: GUI start -> On (2.4 GHz / $Security)."
        $cli = Join-Path ([System.IO.Path]::GetDirectoryName($path)) 'wifi-hotspot-cli.exe'
        $refusal = & $cli --test-cycle --lang en 2>&1
        if ($LASTEXITCODE -eq 0 -or ($refusal -join "`n") -notmatch 'Hardware test refused') {
            throw 'A second process did not refuse the active hotspot.'
        }
        if ((Get-ControlText 1015) -ne $strings.On) { throw 'The second process changed the GUI-owned hotspot.' }
        Write-Output 'PASS: another process refuses to replace/stop the active hotspot.'
        Click-Control 1011
        Wait-Until { (Get-ControlText 1015) -eq $strings.Off } 'hotspot Off' 60
        Write-Output 'PASS: GUI stop -> Off.'
    }
    $rect = [GuiNative+Rect]::new()
    [void][GuiNative]::GetWindowRect($window, [ref]$rect)
    $bitmap = [System.Drawing.Bitmap]::new($rect.Right - $rect.Left, $rect.Bottom - $rect.Top)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    try { if (-not [GuiNative]::PrintWindow($window, $dc, 2)) { throw 'PrintWindow failed' } }
    finally { $graphics.ReleaseHdc($dc) }
    $fullScreenshot = [System.IO.Path]::GetFullPath($Screenshot)
    [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($fullScreenshot))
    $bitmap.Save($fullScreenshot, [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose(); $bitmap.Dispose()
    Write-Output "Screenshot: $fullScreenshot"
    # It is safe to expose the operation log; the program never logs passwords.
    Write-Output (Get-ControlText 1013)
} finally {
    if (-not $LeaveOpen -and -not $process.HasExited -and $window -ne [IntPtr]::Zero) {
        # Graceful close, never kill: the app owns cleanup of any active session.
        [void][GuiNative]::PostMessage($window, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
        if (-not $process.WaitForExit(30000)) { throw 'GUI did not finish graceful close.' }
    }
}
