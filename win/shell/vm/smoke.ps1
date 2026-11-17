# The shell's VM smoke (as the logged-in user, through winps.sh --user-signed), driven from
# outside the app by UI Automation (Windows' own UIAutomationClient), so the app carries no test
# code: the window opens and renders the core's first snapshot, the Manage tab and its buttons
# show the core's texts, the log pane and the log file fill, a core dialog shows and is answered
# (the welcome at start, if the core asks it, then Settings), and closing the window ends the
# process. Screenshots (GDI, from the user's session) go to \\Mac\win\shell\results.
# Exit code: the number of failed checks. Test only.
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing, System.Windows.Forms
$fail = 0
function Check($ok, $what) { if ($ok) { "PASS $what" } else { "FAIL $what"; $script:fail++ } }
$res = "\\Mac\win\shell\results"
New-Item -ItemType Directory -Force $res | Out-Null
function Shot($name) {
  $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
  $bmp.Save("$res\$name.png", [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
}
function Windows($procId) {
  $c = [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ProcessIdProperty, [int]$procId)
  @([System.Windows.Automation.AutomationElement]::RootElement.FindAll([System.Windows.Automation.TreeScope]::Children, $c))
}
function Until($ms, [scriptblock]$cond) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.ElapsedMilliseconds -lt $ms) { $r = & $cond; if ($r) { return $r }; Start-Sleep -Milliseconds 200 }
  return $null
}
function Named($root, $name, $type) {
  $c = [System.Windows.Automation.AndCondition]::new([System.Windows.Automation.Condition[]]@([System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::NameProperty, [string]$name), [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty, $type)))
  $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $c)
}
function Press($el) { $el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
function MainOf($procId) { Windows $procId | Where-Object { Named $_ "Manage" ([System.Windows.Automation.ControlType]::TabItem) } | Select-Object -First 1 }
# a dialog: a window owned by the main window (UI Automation lists it under the main window), or
# a top-level window of the process that isn't the main one
function DialogOf($procId) {
  $m = MainOf $procId
  if ($m) {
    $c = [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Window)
    $d = $m.FindFirst([System.Windows.Automation.TreeScope]::Children, $c)
    if ($d) { return $d }
  }
  Windows $procId | Where-Object { -not (Named $_ "Manage" ([System.Windows.Automation.ControlType]::TabItem)) } | Select-Object -First 1
}
function Texts($el) {
  $c = [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Text)
  @($el.FindAll([System.Windows.Automation.TreeScope]::Descendants, $c) | ForEach-Object { $_.Current.Name })
}
$Btn = [System.Windows.Automation.ControlType]::Button
$settingsName = "Settings" + [char]0x2026   # Windows PowerShell 5.1 reads this file as ANSI: no non-ASCII literal

$exe = "$env:LOCALAPPDATA\Programs\RB Stems Plus\shell-smoke\RB Stems Plus.exe"
$logFile = "$env:LOCALAPPDATA\rbstemsplus\Logs\app.log"
Get-Process "RB Stems Plus" -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item $logFile -ErrorAction SilentlyContinue
$sw = [Diagnostics.Stopwatch]::StartNew()
$app = Start-Process -FilePath $exe -PassThru
$main = Until 20000 { MainOf $app.Id }
Check ($null -ne $main) "the main window opens ($([int]$sw.Elapsed.TotalMilliseconds) ms to UI Automation)"
if (-not $main) { Shot "no-window"; "exit code: $(if ($app.HasExited) { $app.ExitCode } else { 'running' })"; exit 1 }
"window title: $($main.Current.Name)"
Start-Sleep -Milliseconds 800
Shot "1-start"

# the welcome (or any start-up question): the core's dialog, answered with its default button
$d = Until 4000 { DialogOf $app.Id }
if ($d) {
  "start-up dialog: " + ((Texts $d) -join ' | ')
  $buttons = @($d.FindAll([System.Windows.Automation.TreeScope]::Descendants, ([System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty, $Btn))))
  Check ($buttons.Count -ge 1) "the start-up dialog has the core's buttons ($(($buttons | ForEach-Object { $_.Current.Name }) -join ', '))"
  Shot "2-startup-dialog"
  Press $buttons[0]
  Check ($null -ne (Until 5000 { -not (DialogOf $app.Id) })) "the start-up dialog closes when answered"
} else { "no start-up dialog" }

# the Manage tab, its buttons with the core's texts
$tab = Named $main "Manage" ([System.Windows.Automation.ControlType]::TabItem)
Check ($tab -and $tab.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected) "the Manage tab is shown"
foreach ($n in "Install Demucs v4", "Reinstall Demucs v4", "Uninstall Demucs v4", "Install Stems Cache", "Reinstall Stems Cache", $settingsName, "Uninstall Stems Cache") {
  $b = Named $main $n $Btn
  Check ($null -ne $b) "button '$n' (enabled: $(if ($b) { $b.Current.IsEnabled } else { '-' }))"
}
"texts: " + ((Texts $main | Where-Object { $_ }) -join ' | ')

# the log: the pane and the file
$logBox = Named $main "Show Log" ([System.Windows.Automation.ControlType]::Edit)
function LogText { if ($logBox) { $logBox.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value } else { "" } }
$t0 = LogText
Check ($t0.Length -gt 0) "the log pane has lines ($(($t0 -split "`n").Count - 1))"
Check ((Test-Path $logFile) -and (Get-Content $logFile).Count -gt 0) "the log file has lines"
"log (pane, first lines):"; ($t0 -split "`n" | Select-Object -First 12) | ForEach-Object { "  $_" }

# a dialog from a click: Settings...
$settings = Named $main $settingsName $Btn
if ($settings -and $settings.Current.IsEnabled) {
  Press $settings
  $d = Until 8000 { DialogOf $app.Id }
  Check ($null -ne $d) "Settings... shows the core's dialog"
  if ($d) {
    "dialog: " + ((Texts $d) -join ' | ')
    Shot "3-settings-dialog"
    $buttons = @($d.FindAll([System.Windows.Automation.TreeScope]::Descendants, ([System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty, $Btn))))
    "buttons: " + (($buttons | ForEach-Object { $_.Current.Name }) -join ', ')
    $cancel = $buttons | Where-Object { $_.Current.Name -eq "Cancel" } | Select-Object -First 1
    if (-not $cancel) { $cancel = $buttons[-1] }
    Press $cancel
    Check ($null -ne (Until 5000 { -not (DialogOf $app.Id) })) "the dialog closes when answered ($($cancel.Current.Name))"
  }
} else { Check $false "Settings... is enabled" }
Start-Sleep -Milliseconds 500
$t1 = LogText
$fileLines = @(Get-Content $logFile).Count
Check (($t1 -split "`n").Count - 1 -eq $fileLines -and $fileLines -ge 2) "the log pane shows every line the log file has ($fileLines)"
Shot "4-after"

# high contrast while the window is up (WS9): turned on, drawn, turned off again
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Hc {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct HIGHCONTRAST { public int cbSize; public int dwFlags; public IntPtr lpszDefaultScheme; }
  [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  public static extern bool SystemParametersInfo(int action, int param, ref HIGHCONTRAST v, int winIni);
  public static bool Set(bool on) {
    var h = new HIGHCONTRAST { cbSize = Marshal.SizeOf(typeof(HIGHCONTRAST)), dwFlags = on ? 1 : 0 };
    return SystemParametersInfo(0x0043 /* SPI_SETHIGHCONTRAST */, h.cbSize, ref h, 3 /* SPIF_UPDATEINIFILE | SPIF_SENDCHANGE */);
  }
  public static bool On() {
    var h = new HIGHCONTRAST { cbSize = Marshal.SizeOf(typeof(HIGHCONTRAST)) };
    SystemParametersInfo(0x0042 /* SPI_GETHIGHCONTRAST */, h.cbSize, ref h, 0);
    return (h.dwFlags & 1) != 0;
  }
}
"@
$hcOn = [Hc]::Set($true)
Start-Sleep -Seconds 3
Check ($hcOn -and [Hc]::On()) "high contrast turned on while the app runs"
Shot "5-high-contrast"
Check (-not $app.HasExited) "the app keeps running through the theme change"
[Hc]::Set($false) | Out-Null
Start-Sleep -Seconds 3
Check (-not [Hc]::On()) "high contrast turned off again"
Shot "6-high-contrast-off"

# close: the core decides (idle: now), the process ends
$main.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
Check ($app.WaitForExit(10000)) "closing the window ends the app"
if (-not $app.HasExited) { $app.Kill() }
Copy-Item $logFile "$res\app.log" -ErrorAction SilentlyContinue

# ---- the TEST build (TestHooks.cs): About over the welcome, two overlapping dialogs
"== the TEST build"
function OwnedOf($m) {
  $c = [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Window)
  @($m.FindAll([System.Windows.Automation.TreeScope]::Children, $c))
}
function LogHas($s) { (Test-Path $logFile) -and (Select-String -Path $logFile -SimpleMatch $s -Quiet) }
Remove-Item $logFile -ErrorAction SilentlyContinue
$env:RBSP_SHELL_TEST = "about-over-welcome,overlap"
$app = Start-Process -FilePath "$env:LOCALAPPDATA\Programs\RB Stems Plus\shell-smoke-test\RB Stems Plus.exe" -PassThru
$main = Until 20000 { MainOf $app.Id }
Check ($null -ne $main) "the TEST build's window opens"
if ($main) {
  $w = Until 6000 { OwnedOf $main | Where-Object { $_.Current.Name -like "Welcome*" } }
  Check ($null -ne $w) "the welcome is up"
  Check ($null -ne (Until 6000 { LogHas "shell test: About asked" })) "About is asked while the welcome is up"
  Start-Sleep -Milliseconds 800
  $owned = OwnedOf $main
  Check ($owned.Count -eq 1 -and $owned[0].Current.Name -like "Welcome*") "About waits in the queue: one window up, the welcome ($(($owned | ForEach-Object { $_.Current.Name }) -join ', '))"
  Shot "7-about-queued"
  if ($w) { Press ($w.FindFirst([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::NameProperty, "Continue"))) }
  $about = Until 5000 { OwnedOf $main | Where-Object { $_.Current.Name -like "About*" } }
  $owned = OwnedOf $main
  Check ($null -ne $about -and $owned.Count -eq 1) "the welcome answered: About shows next, alone"
  Shot "8-about"
  if ($about) { $ok = Until 3000 { Named $about "OK" $Btn }; if ($ok) { Press $ok } }
  # the overlapping pair, from two threads: one at a time, in call order
  $d1 = Until 8000 { OwnedOf $main | Where-Object { $_.Current.Name -eq "Test dialog 1" } }
  Check ($null -ne (Until 3000 { LogHas "shell test: dialog 2 asked" })) "dialog 2 is asked while dialog 1 is up"
  $owned = OwnedOf $main
  Check ($null -ne $d1 -and $owned.Count -eq 1) "two overlapping dialogs: the first alone ($(($owned | ForEach-Object { $_.Current.Name }) -join ', '))"
  if ($d1) { "dialog 1's buttons: " + ((@($d1.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty, $Btn))) | ForEach-Object { "[" + $_.Current.Name + "]" }) -join ', ') }
  $u = if ($d1) { Until 3000 { Named $d1 "First_One" $Btn } } else { $null }
  Check ($null -ne $u) "a button whose label has a '_' keeps it in its automation name"
  Shot "9-underscore-button"
  if ($u) { Press $u }
  $d2 = Until 5000 { OwnedOf $main | Where-Object { $_.Current.Name -eq "Test dialog 2" } }
  Check ($null -ne $d2) "then the second"
  if ($d2) { $b2 = Until 3000 { Named $d2 "Second" $Btn }; if ($b2) { Press $b2 } }
  Check ($null -ne (Until 5000 { LogHas "shell test: dialog 2 answered 0" })) "the second answered"
  $lines = @(Get-Content $logFile | Where-Object { $_ -like "*shell test: dialog * answered*" })
  Check ($lines.Count -eq 2 -and $lines[0] -like "*dialog 1 answered 0" -and $lines[1] -like "*dialog 2 answered 0") "answered in call order ($($lines -join ' / '))"
  $main.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
  Check ($app.WaitForExit(10000)) "closing the TEST build's window ends it"
}
if (-not $app.HasExited) { $app.Kill() }
Remove-Item Env:\RBSP_SHELL_TEST
Copy-Item $logFile "$res\app-test.log" -ErrorAction SilentlyContinue
"== shell smoke failures: $fail"
exit $fail
