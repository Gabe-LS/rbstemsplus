# V10, as the user with the full token: a rekordbox version removed with Pioneer's own uninstaller
# (its Uninstall entry's UninstallString, silent: /S), while its folder holds the bridge. v10a
# removes 7.2.20 with the watcher stopped first (V3's watcher-off case: the app's own refresh),
# v10b removes 7.2.19. A language picker is answered OK as for the installer (the ruling); any
# other uninstaller window is left alone, the uninstaller ended and the step fails. Then: how
# soon the open app's Manage drops the row (silently: no dialog in the app or the watcher), what
# the uninstaller left in the folder, and the rows left.
$v = if ($VmRun.Step -eq 'v10a') { '7.2.20' } else { '7.2.19' }
$other = if ($v -eq '7.2.20') { '7.2.19' } else { '7.2.20' }
$l = Get-Layout
$wfrom = Get-WatcherLogSize
$app = Get-AppWindow
Check ($null -ne $app) "RB Stems Plus is open (pid $(if ($app) { $app.Id }))"
if (-not $app) { Done }
if ($v -eq '7.2.20') {
    foreach ($w in @(Get-Watchers)) { Stop-Process -Id $w.Id -Force; "the watcher stopped (pid $($w.Id))" }
    Check (Wait-Until { @(Get-Watchers).Count -eq 0 } 15) 'no watcher runs'
}
$e = @(Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall' | ForEach-Object { Get-ItemProperty $_.PSPath } | Where-Object { $_.DisplayName -like "rekordbox $v*" }) | Select-Object -First 1
Check ($null -ne $e) "rekordbox $v's Uninstall entry ($(if ($e) { $e.UninstallString }))"
if (-not $e) { Done }
$cmd = [string]$e.UninstallString
# quoted or not (rekordbox's is "C:\Program Files\...\Uninstall rekordbox.exe", unquoted)
$exe = if ($cmd -match '^"([^"]+)"') { $Matches[1] } elseif ($cmd -match '^(.+?\.exe)\b') { $Matches[1] } else { $cmd }
Check (Test-Path -LiteralPath $exe) "its uninstaller is there ($exe)"
$t0 = Get-Date
$p = Start-Process -FilePath $exe -ArgumentList '/S' -PassThru
"started $exe /S (pid $($p.Id))"
$A = [Windows.Automation.AutomationElement]
function Get-Uninst { @(Get-CimInstance Win32_Process | Where-Object { $_.CreationDate -ge $t0.AddSeconds(-2) -and ($_.ExecutablePath -ieq $exe -or $_.ExecutablePath -match '\\~nsu[^\\]*\\|\\Au_\.exe$|\\Un_[A-Z]\.exe$') }) }
$bad = $false
$t = [Diagnostics.Stopwatch]::StartNew()
while ($t.Elapsed.TotalSeconds -lt 400) {
    $ps = @(Get-Uninst)
    if (-not $ps.Count) { break }
    foreach ($q in $ps) {
        $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, [int]$q.ProcessId)
        foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $cond)) {
            $set = @($w.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition) | ForEach-Object { $_.Current.Name } | Where-Object { $_ } | Sort-Object)
            "uninstaller window '$($w.Current.Name)': $($set -join '; ')"
            if (($set -join '|') -eq "OK|rekordbox was successfully removed from your computer.") {
                # NSIS's end notice, shown even with /S: informational, not a licence, sign-in or UAC dialog
                # its OK (NSIS draws it as a pane with its own window, like the picker's: run 7) clicked
                # with BM_CLICK on that window; a posted IDOK to the box left it open (run 6)
                $ok = @($w.FindAll([Windows.Automation.TreeScope]::Descendants, (New-Object Windows.Automation.PropertyCondition($A::NameProperty, 'OK'))) | Where-Object { $_.Current.NativeWindowHandle -ne 0 }) | Select-Object -First 1
                $h = if ($ok) { [IntPtr]$ok.Current.NativeWindowHandle } else { [IntPtr]::Zero }
                if ($h -ne [IntPtr]::Zero) { [void][RbspMv.U]::PostMessage($h, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero); "the uninstaller's end notice answered OK ($($ok.Current.ControlType.ProgrammaticName))" }
                else { "the uninstaller's end notice: its OK has no window to click" }
                Start-Sleep 2
                continue
            }
            if (($set -join '|') -eq 'Cancel|English|OK|Please select a language.') {
                Add-Type -Namespace W -Name U -MemberDefinition '[DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);' -ErrorAction SilentlyContinue
                [void][W.U]::PostMessage([IntPtr]$w.Current.NativeWindowHandle, 0x0111, [IntPtr]1, [IntPtr]::Zero)
                'the language picker answered OK (English)'
                Start-Sleep 3
            } else {
                Save-MvShot 'uninstaller-window'
                'not the language picker: left alone, the uninstaller ended'
                $ps | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
                $bad = $true
            }
        }
    }
    if ($bad) { break }
    foreach ($r in @(Get-CimInstance Win32_Process -Filter "Name = 'rekordbox.exe'")) { Stop-Process -Id $r.ProcessId -Force -ErrorAction SilentlyContinue; Check $false "rekordbox.exe started (ended: $($r.ExecutablePath))" }
    Start-Sleep -Milliseconds 500
}
$endAt = [Diagnostics.Stopwatch]::StartNew()
Check (-not $bad -and -not @(Get-Uninst).Count) "Pioneer's uninstaller for $v ended by itself ($([int]$t.Elapsed.TotalSeconds) s)"
$d = Get-RbDirOf $v
$left = @(Get-ChildItem -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue)
"left in $($d): $(if (Test-Path -LiteralPath $d) { "$($left.Count) item(s): $(@($left | Select-Object -First 15 | ForEach-Object { $_.FullName.Substring($d.Length) }) -join ', ')" } else { 'the folder is gone' })"
$bridgeLeft = (Get-Sha (Get-RbOrtOf $v)) -eq (Get-ServedBridge)
"the bridge left there: $bridgeLeft"
$gone = Rx (Get-RowText 'T_WIN_ROW_ON' $v)
if ($bridgeLeft) {
    $seen = Wait-MvNames $app.Id @((Rx (Get-RowText 'T_WIN_ROW_REPAIR' $v))) 120
    Check $seen "a leftover folder holding the bridge shows $(Get-RowText 'T_WIN_ROW_REPAIR' $v) ($([math]::Round($endAt.Elapsed.TotalSeconds, 1)) s)"
} else {
    $seen = Wait-MvNames $app.Id @() 120 @($gone, (Rx (Get-RowText 'T_WIN_ROW_REPAIR' $v)), (Rx (Get-RowText 'T_WIN_ROW_READY' $v)))
    Check $seen "the $v row is gone ($([math]::Round($endAt.Elapsed.TotalSeconds, 1)) s after the uninstaller ended)"
}
Show-MvRows $app.Id
if ($v -eq '7.2.20') {
    # one version left: the compact layout again (no rows), Stems Cache still on in it
    Check (Wait-MvNames $app.Id @() 30 @(' \u00B7 ')) "one version left: the compact layout (no version rows)"
    Check ((Test-CacheOn) -and (Get-Sha (Get-RbOrtOf $other)) -eq (Get-ServedBridge)) "$other still on (its DLL the bridge, rekordbox_model=1)"
} elseif (-not (Test-Path -LiteralPath (Get-RbDirOf '7.2.20')) -and -not $bridgeLeft) {
    Check (Find-UiText $app.Id (Rx (Get-CoreText 'T_RB_NOT_INSTALLED')) 60) "Manage says: $(Get-CoreText 'T_RB_NOT_INSTALLED')"
}
Start-Sleep 5
Check ((Get-MvWindowCount $app.Id) -eq 1) 'silently: no dialog in the app'
$asks = @((Read-WatcherLog $wfrom) -split "`r?`n" | Where-Object { $_ -match 'asking about' })
Check ($asks.Count -eq 0) "and none from the watcher ($($asks.Count))"
Save-MvShot 'after'
foreach ($x in ((Read-WatcherLog $wfrom) -split "`r?`n")) { if ($x) { "  watcher.log: $x" } }
"watchers now: $(@(Get-Watchers | ForEach-Object Id) -join ', ')"
Done
