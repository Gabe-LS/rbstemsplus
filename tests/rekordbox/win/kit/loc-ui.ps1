# loc-ui.ps1: plan N3's PC rows in our app's own UI (coordinator 2026-11-20), dot-sourced by
# session-task.ps1 for its "loc STEP|ARG" command, in the test account's desktop session:
#   set|FOLDER   Settings > Change..., FOLDER typed into the folder picker, Select Folder, then our
#                dialogs followed: its Use "DRIVE" (or Use Them) pressed, an alert's OK read and pressed
#   thispc       Settings > Use This PC, its confirmation's Use This PC pressed
#   pick         Settings > Change..., the picker shown, then Cancel (how long each took)
#   texts        the location lines the Status, Prepare and Manage tabs show, each tab's read timed
#   install      the Stems Cache install or reinstall confirmation read, then Cancel (never past it,
#                so nothing is elevated)
#   uninstall    our app's complete uninstall: its confirmation's text read, then Uninstall, Done
#   netmap|L     a hung mapped drive in this account only: HKCU\Network\L to share x on TEST-NET-1 .1 (RFC 5737:
#                nothing answers) and this session's own letter L: to it (DefineDosDevice, no
#                connection made), both refused when L is in use
#   netunmap|L   both removed again, and checked gone
# Every other dialog is answered Cancel and the step stops. Never rekordbox's windows, never a UAC,
# licence or security prompt. One answer line; its events timed from the step's start, ' || ' apart.
$LocNet = '\\' + (@(192, 0, 2, 1) -join '.') + '\x'   # TEST-NET-1's first address (RFC 5737), share x

function Loc-Ev([string]$s) { $script:LocEv.Add(('{0:0.0}s {1}' -f $script:LocT.Elapsed.TotalSeconds, $s)) }
function Loc-Cut([string]$s, [int]$n = 300) {
    $s = ($s -replace '\s+', ' ').Trim()
    if ($s.Length -gt $n) { return $s.Substring(0, $n) + '...' }
    return $s
}
function Loc-All($el, $type) {
    $A = [Windows.Automation.AutomationElement]
    @($el.FindAll([Windows.Automation.TreeScope]::Descendants, (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $type))))
}
# our process's windows other than its main one, and the windows inside it (its dialogs), each once
function Loc-Windows($app) {
    $A = [Windows.Automation.AutomationElement]; $CT = [Windows.Automation.ControlType]
    $pc = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $app.Id)
    $seen = @{}; $out = New-Object System.Collections.Generic.List[object]
    foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $pc)) {
        foreach ($d in @($w) + @(Loc-All $w $CT::Window)) {
            if ([IntPtr]$d.Current.NativeWindowHandle -eq $app.MainWindowHandle) { continue }
            $k = ($d.GetRuntimeId() -join '.')
            if (-not $seen.ContainsKey($k)) { $seen[$k] = 1; $out.Add($d) }
        }
    }
    return $out
}
function Loc-Text($w) { (@(Loc-All $w ([Windows.Automation.ControlType]::Text) | ForEach-Object { $_.Current.Name } | Where-Object { $_ }) -join ' / ') }
function Loc-Buttons($w) { @(Loc-All $w ([Windows.Automation.ControlType]::Button) | Where-Object { $_.Current.Name }) }
function Loc-Btn($w, [string]$name) { Loc-Buttons $w | Where-Object { $_.Current.Name -eq $name } | Select-Object -First 1 }
# a dialog's control by its control ID (GetDlgItem: the picker's Choose 1, Cancel 2 aren't in its UI
# Automation subtree on the PC, 2026-11-20), else by AutomationId
function Loc-ById($w, [string]$id) {
    $A = [Windows.Automation.AutomationElement]
    $h = [IntPtr]$w.Current.NativeWindowHandle
    if ($h -ne [IntPtr]::Zero) { $c = [RbspLoc.Dos]::GetDlgItem($h, [int]$id); if ($c -ne [IntPtr]::Zero) { return $A::FromHandle($c) } }
    $w.FindFirst([Windows.Automation.TreeScope]::Descendants, (New-Object Windows.Automation.AndCondition(
        (New-Object Windows.Automation.PropertyCondition($A::AutomationIdProperty, $id)),
        (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, [Windows.Automation.ControlType]::Button)))))
}
# a dialog's button ID pressed as the dialog itself takes it: WM_COMMAND (BN_CLICKED) posted to the
# dialog (UI Automation's Invoke on the picker's Cancel did nothing on the PC, 2026-11-20)
function Loc-Press($w, [int]$id) {
    $h = [IntPtr]$w.Current.NativeWindowHandle; if ($h -eq [IntPtr]::Zero) { return $false }
    $c = [RbspLoc.Dos]::GetDlgItem($h, $id); if ($c -eq [IntPtr]::Zero) { return $false }
    return [RbspLoc.Dos]::PostMessage($h, 0x0111, [IntPtr]$id, $c)
}
# the folder picker (IFileOpenDialog: a #32770 with its folder box) and that box: the edit in control
# 1148 (a ComboBoxEx32's combo box) or control 1152, found by window handles
function Loc-PickerEdit($p) {
    $h = [IntPtr]$p.Current.NativeWindowHandle; if ($h -eq [IntPtr]::Zero) { return $null }
    $e = [IntPtr]::Zero
    $cb = [RbspLoc.Dos]::GetDlgItem($h, 1148)
    if ($cb -ne [IntPtr]::Zero) {
        $e = [RbspLoc.Dos]::FindWindowEx($cb, [IntPtr]::Zero, 'Edit', $null)
        if ($e -eq [IntPtr]::Zero) { $c2 = [RbspLoc.Dos]::FindWindowEx($cb, [IntPtr]::Zero, 'ComboBox', $null); if ($c2 -ne [IntPtr]::Zero) { $e = [RbspLoc.Dos]::FindWindowEx($c2, [IntPtr]::Zero, 'Edit', $null) } }
    }
    if ($e -eq [IntPtr]::Zero) { $e = [RbspLoc.Dos]::GetDlgItem($h, 1152) }
    if ($e -eq [IntPtr]::Zero) { return $null }
    return [Windows.Automation.AutomationElement]::FromHandle($e)
}
function Loc-Picker($app) { Loc-Windows $app | Where-Object { $_.Current.ClassName -eq '#32770' -and (Loc-PickerEdit $_) } | Select-Object -First 1 }
function Loc-Label($w) { (@(Loc-All $w ([Windows.Automation.ControlType]::Text) | ForEach-Object { $_.Current.Name } | Where-Object { $_ -like 'Stems Cache location*' }) -join ' / ') }
# our core's dialog showing now (a window with buttons, not the picker or its message boxes)
function Loc-Dialog($app) {
    foreach ($w in @(Loc-Windows $app)) { if ($w.Current.ClassName -ne '#32770' -and @(Loc-Buttons $w).Count) { return $w } }
    return $null
}
# Settings opened from the menu (RB Stems Plus > Settings...): its window
function Loc-Settings($app) {
    $CT = [Windows.Automation.ControlType]; $dots = [char]0x2026
    [void](Invoke-El (Find-Named $app 'Continue' $CT::Button 1))   # a welcome still open
    $top = Find-El $app 'RB Stems Plus' $CT::MenuItem 10; $p = $null
    if (-not ($top -and $top.TryGetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$p))) { throw 'no RB Stems Plus menu' }
    $p.Expand()
    if (-not (Invoke-El (Find-El $app "Settings$dots" $CT::MenuItem 10))) { throw 'no Settings... menu item' }
    $t = [Diagnostics.Stopwatch]::StartNew()
    while ($t.Elapsed.TotalSeconds -lt 30) {
        $d = Loc-Dialog $app
        if ($d -and (Loc-Btn $d "Change$dots")) { Loc-Ev "Settings: $(Loc-Label $d)"; return $d }
        Start-Sleep -Milliseconds 300
    }
    throw 'no Settings dialog in 30 s'
}
# our dialogs followed until Settings is back (then Cancel) or none shows for 20 s: each one's text
# logged; a button matching $yes pressed, a lone OK pressed; anything else Cancel and $false
function Loc-Follow($app, [string[]]$yes, [int]$secs) {
    $dots = [char]0x2026; $quiet = $null; $last = ''; $pickerSaid = $false
    while ($script:LocT.Elapsed.TotalSeconds -lt $secs) {
        $app.Refresh(); if ($app.HasExited) { Loc-Ev 'the app exited'; return $false }
        $d = Loc-Dialog $app
        if (-not $d) {
            $mb = Loc-Windows $app | Where-Object { $_.Current.ClassName -eq '#32770' -and -not (Loc-PickerEdit $_) } | Select-Object -First 1
            $pk = Loc-Picker $app
            if ($mb) {
                Loc-Ev "the picker's own message: $(Loc-Cut (Loc-Text $mb))"
                if (-not (Loc-Press $mb 2)) { [void](Loc-Press $mb 1) }; Start-Sleep 1
                $pk = Loc-Picker $app
                if ($pk) { [void](Loc-Press $pk 2); Loc-Ev 'the picker cancelled' }
                $quiet = $null; continue
            }
            if ($pk) { if (-not $pickerSaid) { Loc-Ev 'the picker still open'; $pickerSaid = $true }; $quiet = $null; Start-Sleep -Milliseconds 300; continue }
            if (-not $quiet) { $quiet = [Diagnostics.Stopwatch]::StartNew() }
            elseif ($quiet.Elapsed.TotalSeconds -ge 20) { Loc-Ev 'no dialog for 20 s'; return $true }
            Start-Sleep -Milliseconds 300; continue
        }
        $quiet = $null
        $names = @(Loc-Buttons $d | ForEach-Object { $_.Current.Name })
        if ($names -contains "Change$dots") { Loc-Ev "Settings again: $(Loc-Label $d)"; [void](Invoke-El (Loc-Btn $d 'Cancel')); return $true }
        $key = "$($d.Current.Name)|$($names -join ',')|$(Loc-Text $d)"
        if ($key -eq $last) { Start-Sleep -Milliseconds 300; continue }   # the one just answered, still closing
        $last = $key
        Loc-Ev "dialog '$($d.Current.Name)': $(Loc-Cut (Loc-Text $d)) [buttons: $($names -join ', ')]"
        $b = $null
        foreach ($y in $yes) { $b = Loc-Buttons $d | Where-Object { $_.Current.Name -like $y } | Select-Object -First 1; if ($b) { break } }
        if (-not $b -and $names.Count -eq 1 -and $names[0] -eq 'OK') { $b = Loc-Btn $d 'OK' }
        if ($b) { Loc-Ev "pressed '$($b.Current.Name)'"; [void](Invoke-El $b); Start-Sleep -Milliseconds 700; continue }
        Loc-Ev 'not a dialog this step answers: Cancel'; [void](Invoke-El (Loc-Btn $d 'Cancel')); return $false
    }
    Loc-Ev "not done in $secs s"; return $false
}
function Loc-WaitPicker($app, [int]$secs) {
    $t = [Diagnostics.Stopwatch]::StartNew()
    while ($t.Elapsed.TotalSeconds -lt $secs) { $p = Loc-Picker $app; if ($p) { Loc-Ev 'the picker shown'; return $p }; Start-Sleep -Milliseconds 300 }
    throw "no folder picker in $secs s"
}
# the location lines a tab shows (names and help texts), its read timed
function Loc-TabLines($app, [string]$tab) {
    $A = [Windows.Automation.AutomationElement]; $CT = [Windows.Automation.ControlType]
    $t = [Diagnostics.Stopwatch]::StartNew()
    $ti = Find-El $app $tab $CT::TabItem 10
    if (-not $ti) { Loc-Ev "${tab}: no tab"; return }
    [void](Select-El $ti); Start-Sleep 2
    $main = $A::FromHandle($app.MainWindowHandle); $lines = New-Object System.Collections.Generic.List[string]
    foreach ($e in $main.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)) {
        if ($e.Current.ControlType -eq $CT::DataItem) { continue }   # the tables' rows (tracks, kept sets)
        foreach ($s in $e.Current.Name, $e.Current.HelpText) {
            if ($s -and $s -match 'Stems Cache|drive|checking|checked|connected|stopped saving|this PC|location' -and -not $lines.Contains($s)) { $lines.Add($s) }
        }
    }
    Loc-Ev "${tab} read in $([math]::Round($t.Elapsed.TotalSeconds - 2, 1)) s (after 2 s shown): $(Loc-Cut (($lines | Select-Object -First 8) -join ' / ') 700)"
}
Add-Type -Namespace RbspLoc -Name Dos -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool DefineDosDevice(uint flags, string device, string target);
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern uint QueryDosDevice(string device, System.Text.StringBuilder target, int max);
[DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr dialog, int id);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr hwnd, uint flags);
[DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")] public static extern IntPtr GetText(IntPtr hwnd, uint msg, IntPtr max, System.Text.StringBuilder text);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr hwnd, uint msg, IntPtr w, string l);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
'@ -ErrorAction SilentlyContinue
function Loc-DosTarget([string]$dev) { $sb = New-Object Text.StringBuilder 1024; if ([RbspLoc.Dos]::QueryDosDevice($dev, $sb, 1024)) { return $sb.ToString() }; return '' }

function Invoke-LocStep([string]$spec) {
    $script:LocT = [Diagnostics.Stopwatch]::StartNew(); $script:LocEv = New-Object System.Collections.Generic.List[string]
    $step, $sarg = $spec -split '\|', 2
    $ok = $false; $dots = [char]0x2026
    try {
        Uia; $CT = [Windows.Automation.ControlType]
        if ($step -in 'netmap', 'netunmap') {
            if ($sarg -notmatch '^[G-Z]$') { throw "not a drive letter G-Z: '$sarg'" }
            $dev = "${sarg}:"; $key = "HKCU:\Network\$sarg"; $target = '\Device\Mup' + $LocNet.Substring(1)
            if ($step -eq 'netmap') {
                if ((Test-Path -LiteralPath $key) -or (Loc-DosTarget $dev)) { throw "$dev is in use here (HKCU\Network or a device): refused" }
                [void](New-Item -Path $key -Force)
                foreach ($v in @(@('RemotePath', $LocNet, 'String'), @('UserName', '', 'String'), @('ProviderName', 'Microsoft Windows Network', 'String'),
                        @('ProviderType', 0x20000, 'DWord'), @('ConnectionType', 1, 'DWord'), @('DeferFlags', 4, 'DWord'))) {
                    [void](New-ItemProperty -Path $key -Name $v[0] -Value $v[1] -PropertyType $v[2] -Force) }
                if (-not [RbspLoc.Dos]::DefineDosDevice(1, $dev, $target)) { throw "DefineDosDevice failed ($([Runtime.InteropServices.Marshal]::GetLastWin32Error()))" }
                Loc-Ev "HKCU\Network\$sarg -> $LocNet; $dev -> $(Loc-DosTarget $dev)"
            } else {
                $had = Loc-DosTarget $dev
                if ($had -eq $target) { [void][RbspLoc.Dos]::DefineDosDevice(7, $dev, $target) }
                elseif ($had) { Loc-Ev "$dev points at '$had', not ours: left alone" }
                if (Test-Path -LiteralPath $key) {
                    if ((Get-ItemProperty -LiteralPath $key).RemotePath -eq $LocNet) { Remove-Item -LiteralPath $key -Recurse -Force } else { Loc-Ev "HKCU\Network\$sarg isn't ours: left alone" } }
                $gone = -not (Test-Path -LiteralPath $key) -and -not (Loc-DosTarget $dev)
                Loc-Ev "removed: registry and device gone $gone"
                if (-not $gone) { throw 'not all removed' }
            }
            $ok = $true
        } else {
            $app = Get-App; if (-not $app) { throw 'our app is not running and could not be started' }
            Restore-App $app
            if ($step -ne 'texts' -and (Get-RbProcess $sid)) { throw 'rekordbox is running in this session: close it first' }
            switch ($step) {
                'set' {
                    $sw = Loc-Settings $app; $chg = Loc-Btn $sw "Change$dots"
                    if (-not $chg.Current.IsEnabled) { [void](Invoke-El (Loc-Btn $sw 'Cancel')); throw 'Change... is disabled' }
                    [void](Invoke-El $chg); $p = Loc-WaitPicker $app 60
                    $e = Loc-PickerEdit $p; $vp = $null
                    if (-not $e) { throw 'the picker has no folder box' }
                    # typed as a user types it (a path set by message was ignored on the PC, 2026-11-20), keys sent
                    # only while the picker is the foreground window. The picker opens in the folder used last, and
                    # a typed path to the folder it is in changes nothing: so the parent typed (Enter goes there),
                    # then the folder's name (Enter goes into it, the box then holds its name), then Enter chooses
                    # it, again 3 s apart while the picker is open (3 at most). A drive's root is typed as it is.
                    Add-Type -AssemblyName System.Windows.Forms
                    $leaf = Split-Path -Leaf $sarg; $parent = Split-Path -Parent $sarg
                    $typed = @(if ($parent -and $leaf) { $parent, $leaf } else { $sarg })
                    function Loc-Keys($what) {
                        $p2 = Loc-Picker $app; if (-not $p2) { return $false }
                        $ph = [IntPtr]$p2.Current.NativeWindowHandle; $e2 = Loc-PickerEdit $p2
                        try { $e2.SetFocus() } catch { }
                        [void][RbspLoc.Dos]::SetForegroundWindow($ph); Start-Sleep -Milliseconds 300
                        $fg = [RbspLoc.Dos]::GetAncestor([RbspLoc.Dos]::GetForegroundWindow(), 2)
                        if ($fg -ne $ph) { [void](Loc-Press $p2 2); throw "the picker is not the foreground window (0x$($fg.ToString('X'))): no keys sent, the picker cancelled" }
                        if ($null -ne $what) { [Windows.Forms.SendKeys]::SendWait('^a'); [Windows.Forms.SendKeys]::SendWait(($what -replace '([+^%~(){}\[\]])', '{$1}')) }
                        $sb = New-Object Text.StringBuilder 600; [void][RbspLoc.Dos]::GetText([IntPtr]$e2.Current.NativeWindowHandle, 0x000D, [IntPtr]600, $sb)
                        [Windows.Forms.SendKeys]::SendWait('{ENTER}')
                        Loc-Ev "Enter pressed (the box '$sb')"; Start-Sleep 3
                        return $true
                    }
                    foreach ($t in $typed) { if (-not (Loc-Keys $t)) { break } }
                    for ($i = 1; $i -le 3; $i++) { if (-not (Loc-Keys $null)) { break } }
                    $ok = Loc-Follow $app @('Use "*"', 'Use Them') 240
                }
                'thispc' {
                    $sw = Loc-Settings $app; $u = Loc-Btn $sw 'Use This PC'
                    if (-not ($u -and $u.Current.IsEnabled)) { [void](Invoke-El (Loc-Btn $sw 'Cancel')); throw 'Use This PC is disabled' }
                    [void](Invoke-El $u); $ok = Loc-Follow $app @('Use This PC') 180
                }
                'pick' {
                    $sw = Loc-Settings $app; [void](Invoke-El (Loc-Btn $sw "Change$dots"))
                    $p = Loc-WaitPicker $app 120; Start-Sleep 3
                    if (-not (Loc-Press $p 2)) { throw 'the picker has no button 2' }; Loc-Ev 'Cancel pressed in the picker'
                    $ok = Loc-Follow $app @() 180
                }
                'texts' { foreach ($tab in 'Status', 'Prepare', 'Manage') { Loc-TabLines $app $tab }; $ok = $true }
                'windows' {
                    # read only: our process's windows, their buttons and edits (name#AutomationId, enabled)
                    foreach ($w in @(Loc-Windows $app)) {
                        $bs = @(Loc-All $w $CT::Button | Where-Object { $_.Current.AutomationId -match '^\d*$' } | Select-Object -First 16 | ForEach-Object { "$($_.Current.Name)#$($_.Current.AutomationId)$(if (-not $_.Current.IsEnabled) { '(off)' })" })
                        $es = @(@(Loc-All $w $CT::Edit) + @(Loc-All $w $CT::ComboBox) | Where-Object { $_.Current.AutomationId -notlike 'System.*' } | Select-Object -First 10 | ForEach-Object { "$($_.Current.ControlType.ProgrammaticName -replace 'ControlType\.', '') $($_.Current.Name)#$($_.Current.AutomationId)" })
                        Loc-Ev "window '$($w.Current.Name)' class $($w.Current.ClassName) handle $($w.Current.NativeWindowHandle): buttons $($bs -join ', '); edits $($es -join ', ')"
                    }
                    $pk = Loc-Picker $app
                    if ($pk) {
                        $pe = Loc-PickerEdit $pk; $b1 = Loc-ById $pk '1'; $b2 = Loc-ById $pk '2'
                        Loc-Ev "picker: folder box $(if ($pe) { "'$($pe.Current.Name)'" } else { 'not found' }); 1 '$(if ($b1) { $b1.Current.Name })'; 2 '$(if ($b2) { $b2.Current.Name })'"
                        if ($sarg -eq 'cancel' -and $b2) { [void](Loc-Press $pk 2); Loc-Ev 'the picker cancelled'; [void](Loc-Follow $app @() 60) }
                    }
                    Loc-Ev "main handle $($app.MainWindowHandle); $(Shot)"; $ok = $true
                }
                'install' {
                    $ti = Find-El $app 'Manage' $CT::TabItem 5; if ($ti) { [void](Select-El $ti) }
                    $b = Find-El $app 'Reinstall Stems Cache' $CT::Button 5 -Enabled
                    if (-not $b) { $b = Find-El $app 'Install Stems Cache' $CT::Button 5 -Enabled }
                    if (-not $b) { throw 'no enabled Install or Reinstall Stems Cache button' }
                    Loc-Ev "pressed '$($b.Current.Name)'"; [void](Invoke-El $b)
                    $t = [Diagnostics.Stopwatch]::StartNew(); $d = $null
                    while (-not $d -and $t.Elapsed.TotalSeconds -lt 60) { $d = Loc-Dialog $app; if (-not $d) { Start-Sleep -Milliseconds 300 } }
                    if (-not $d) { throw 'no confirmation in 60 s' }
                    Loc-Ev "dialog '$($d.Current.Name)': $(Loc-Cut (Loc-Text $d) 700) [buttons: $((@(Loc-Buttons $d | ForEach-Object { $_.Current.Name })) -join ', ')]"
                    $c = Loc-Btn $d 'Cancel'; if (-not $c) { throw 'no Cancel in it' }
                    [void](Invoke-El $c); Loc-Ev 'Cancel pressed'; $ok = $true
                }
                'uninstall' {
                    $top = Find-El $app 'RB Stems Plus' $CT::MenuItem 10; $p = $null
                    if (-not ($top -and $top.TryGetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$p))) { throw 'no RB Stems Plus menu' }
                    $p.Expand()
                    if (-not (Invoke-El (Find-El $app "Uninstall RB Stems Plus$dots" $CT::MenuItem 10))) { throw 'no Uninstall RB Stems Plus... menu item' }
                    $t = [Diagnostics.Stopwatch]::StartNew(); $d = $null
                    while ($t.Elapsed.TotalSeconds -lt 60) { $d = Loc-Dialog $app; if ($d -and (Loc-Btn $d 'Uninstall')) { break }; $d = $null; Start-Sleep -Milliseconds 300 }
                    if (-not $d) { throw 'no Uninstall confirmation in 60 s' }
                    Loc-Ev "dialog '$($d.Current.Name)': $(Loc-Cut (Loc-Text $d) 700)"
                    [void](Invoke-El (Loc-Btn $d 'Uninstall')); Loc-Ev 'Uninstall pressed'
                    $t.Restart(); $dn = $null
                    while (-not $dn -and $t.Elapsed.TotalSeconds -lt 180) { $d = Loc-Dialog $app; if ($d) { $dn = Loc-Btn $d 'Done' }; if (-not $dn) { Start-Sleep -Milliseconds 500 } }
                    if (-not $dn) { throw "no Done in 180 s; $(Shot)" }
                    Loc-Ev "done dialog: $(Loc-Cut (Loc-Text $d) 700)"; [void](Invoke-El $dn)
                    $t.Restart(); while (-not $app.HasExited -and $t.Elapsed.TotalSeconds -lt 90) { Start-Sleep -Milliseconds 500; $app.Refresh() }
                    Loc-Ev "the app exited $($app.HasExited)"; $ok = $app.HasExited
                }
                default { throw "unknown step '$step'" }
            }
        }
    } catch { Loc-Ev ('FAILED: ' + ($_.Exception.Message -replace '[\r\n]+', ' ')) }
    return "loc ${step}: $(if ($ok) { 'ok' } else { 'not ok' }) || " + ($script:LocEv -join ' || ')
}
