# Desktop-only UI for RBSP Formats. A new or ambiguous control/window stops the task.
function Get-FmtHandle([string]$line) {
    [IntPtr][Convert]::ToInt64(($line -split ' \| ')[-1].Substring(2), 16)
}

# Source entries are the top-level items and their immediate children in a UIA Tree.
function Get-FmtTreePosition($el, $walker) {
    $level = 0
    for ($i = 0; $i -lt 12; $i++) {
        $el = $walker.GetParent($el)
        if (-not $el -or $el.Current.ProcessId -ne $fmtPid) { return }
        switch ($el.Current.ControlType.ProgrammaticName) {
            'ControlType.Tree' { return @{ Tree = $el; Level = $level } }
            'ControlType.TreeItem' { $level++ }
            'ControlType.Window' { return }
        }
    }
}

function Get-FmtSourceName([string]$name) {
    $name -creplace ' \([0-9]+\)\z', ''
}

function Get-FmtMenuCaption([string]$name) {
    ($name -creplace '\t(?:Ctrl|Control)\+O\z', '') -creplace '(?:\.\.\.|\u2026)\z', ''
}

function Get-FmtUiName($c, [int]$level = -1, [switch]$Popup) {
    $type = $c.ControlType.ProgrammaticName
    if ($type -cin 'ControlType.Button', 'ControlType.TabItem', 'ControlType.MenuItem', 'ControlType.MenuBar',
        'ControlType.Menu', 'ControlType.ToolBar', 'ControlType.Pane', 'ControlType.Window',
        'ControlType.Header', 'ControlType.HeaderItem') {
        if ((Get-FmtMenuCaption $c.Name) -cin '', 'menu', 'File', 'Import', 'Import Track', 'Import Folder',
            'Import Playlist', 'Open', 'Save', 'Save As', 'Cancel', 'OK', 'Close', 'Yes', 'No', 'rekordbox', 'Import Error',
            'Delete Playlist', 'Remove', 'Remove from Collection', 'Remove from Playlist', 'Create New Playlist',
            'File name:', 'Export', 'Export Playlist', 'Export a playlist to a file', 'Export a playlist to a file (*.txt)',
            'Export a playlist to a file for music apps (*.m3u8)') { return $c.Name }
        if (-not $Popup -and $type -ceq 'ControlType.Button' -and
            $c.Name -cin 'ShowHideTreeShortcutButton', 'SearchPlaylistButton') { return $c.Name }
    }
    if ($type -cin 'ControlType.Custom', 'ControlType.Text' -or
        ($type -ceq 'ControlType.TreeItem' -and $level -in 0, 1)) {
        $allowed = [string[]]@('Collection', 'All', 'Date Added', 'Genre', 'Artist', 'Album',
            'Playlists', 'All Tracks', 'RBSP Formats', 'RBSP Tests')
        if ([array]::IndexOf($allowed, (Get-FmtSourceName $c.Name)) -ge 0) { return $c.Name }
    }
    return '<redacted>'
}

function Protect-FmtUiMetadata([string]$value, [string[]]$privateNames) {
    # Some providers build IDs from UI names, including names of ancestor controls.
    foreach ($name in $privateNames) {
        if ($name -and $value.IndexOf($name, [StringComparison]::OrdinalIgnoreCase) -ge 0) { return '<redacted>' }
    }
    return $value
}

function Get-FmtUiIdentity($c, [string]$name, [string[]]$privateNames) {
    [ordered]@{
        ControlType = $c.ControlType.ProgrammaticName
        Name = $name
        AutomationId = Protect-FmtUiMetadata $c.AutomationId $privateNames
        IsEnabled = $c.IsEnabled
        IsOffscreen = $c.IsOffscreen
    }
}

function Get-FmtUiDump($root, $walker, [int]$limit = 3000, [switch]$Popup, [string]$Capture) {
    $budget = @{ Remaining = $limit }
    function Walk-FmtUi($el, [int]$depth, [int]$level, [string[]]$privateNames, [bool]$popupScope) {
        $budget.Remaining--
        $c = $el.Current
        if ($c.ProcessId -ne $fmtPid) { return }
        $type = $c.ControlType.ProgrammaticName
        if ($depth -and $type -ceq 'ControlType.Window') { $popupScope = $true }
        $name = Get-FmtUiName $c $level -Popup:$popupScope
        if ($name -ceq '<redacted>' -and $c.Name) { $privateNames += $c.Name }
        $row = Get-FmtUiIdentity $c $name $privateNames
        if ($Capture) { $row['Capture'] = $Capture }
        if ($popupScope -and $type -ceq 'ControlType.MenuItem' -and (Get-FmtMenuCaption $name) -ceq 'Import Track' -and
            $c.AcceleratorKey -match '^(?:Ctrl|Control)\+O$') { $row['AcceleratorKey'] = 'Ctrl+O' }
        $row['Depth'] = $depth
        $row['ClassName'] = Protect-FmtUiMetadata $c.ClassName $privateNames
        $row['FrameworkId'] = Protect-FmtUiMetadata $c.FrameworkId $privateNames
        $row['BoundingRectangle'] = $c.BoundingRectangle.ToString()
        $row | ConvertTo-Json -Compress
        if ($depth -ge 12 -or $budget.Remaining -le 0) { return }
        if ($type -ceq 'ControlType.Tree') { $level = 0 }
        elseif ($type -ceq 'ControlType.Window') { $level = -1 }
        elseif ($type -ceq 'ControlType.TreeItem' -and $level -ge 0) { $level++ }
        $child = $walker.GetFirstChild($el)
        while ($child -and $budget.Remaining -gt 0) {
            Walk-FmtUi $child ($depth + 1) $level $privateNames $popupScope
            if ($budget.Remaining -gt 0) { $child = $walker.GetNextSibling($child) }
        }
    }
    Walk-FmtUi $root 0 -1 @() $Popup.IsPresent
}

function Get-FmtMainElement([IntPtr]$handle, [int]$processId) {
    $main = [Windows.Automation.AutomationElement]::FromHandle($handle)
    if ($main.Current.Name -cne 'rekordbox' -or $main.Current.ProcessId -ne $processId -or
        [int64]$main.Current.NativeWindowHandle -ne $handle.ToInt64()) { throw 'formats: UI Automation main window did not match' }
    return $main
}

function Save-FmtUiDump {
    Uia
    $handle, $fmtPid = Get-RbMain
    $main = Get-FmtMainElement $handle $fmtPid
    $lines = @(Get-FmtUiDump $main ([Windows.Automation.TreeWalker]::ControlViewWalker))
    if (-not $lines.Count) { throw 'formats: the UI Automation tree was empty' }
    $file = Write-FmtUiDump $lines
    return "fmtuidump: OK $($lines.Count) $file"
}

function Write-FmtUiDump([string[]]$lines) {
    $file = 'format-uidump-' + [guid]::NewGuid().ToString('N') + '.jsonl'
    $path = Join-Path $B $file
    Assert-FormatPath $path
    Set-Content -LiteralPath $path -Value $lines -Encoding UTF8
    return $file
}

function Save-FmtStopDump([switch]$Step) {
    if (-not [RbspWin]::OrdinaryDesktop() -or [RbspWin]::Pid($fmtHandle) -ne $fmtPid) { return }
    $walker = [Windows.Automation.TreeWalker]::ControlViewWalker
    $lines = [Collections.Generic.List[string]]::new()
    if ($Step -and $fmtStep.Mode) {
        $lines.Add(([ordered]@{ Capture = 'main'; Mode = $fmtStep.Mode } | ConvertTo-Json -Compress))
    }
    foreach ($line in @(Get-DesktopWindows -AllSizes)) {
        if (-not $line.StartsWith('rekordbox | ')) { continue }
        $h = Get-FmtHandle $line
        if ($h -eq $fmtHandle -or [RbspWin]::Pid($h) -ne $fmtPid) { continue }
        try {
            $popup = [Windows.Automation.AutomationElement]::FromHandle($h)
            if ($popup.Current.ProcessId -eq $fmtPid -and [int64]$popup.Current.NativeWindowHandle -eq $h.ToInt64()) {
                $capture = if ($Step) { 'window-0x' + $h.ToString('X') }
                $lines.AddRange([string[]]@(Get-FmtUiDump $popup $walker (500 - $lines.Count) -Popup -Capture $capture))
            }
        } catch { }
        if ($lines.Count -ge 500) { break }
    }
    $capture = if ($Step) { 'main' }
    $lines.AddRange([string[]]@(Get-FmtUiDump $fmtMain $walker (3000 - $lines.Count) -Capture $capture))
    if ($lines.Count) {
        $file = Write-FmtUiDump $lines.ToArray()
        if ($Step) { return "$($lines.Count) $file" }
        return $file
    }
}

function Save-FmtCloseDump([int]$fmtPid) {
    Initialize-RbspWin
    if (-not [RbspWin]::OrdinaryDesktop()) { throw 'ordinary input desktop unavailable' }
    Uia
    $walker = [Windows.Automation.TreeWalker]::ControlViewWalker
    $handles = @([RbspWin]::HandlesForProcess($fmtPid))
    if (-not $handles.Count) { throw 'no rekordbox windows for the close dump' }
    $limit = [Math]::Max(1, [int][Math]::Floor(3000 / $handles.Count))
    $lines = @(foreach ($h in $handles) {
        if ([RbspWin]::Pid($h) -ne $fmtPid) { continue }
        $capture = 'close-0x' + $h.ToString('X')
        $tree = @(try {
            $root = [Windows.Automation.AutomationElement]::FromHandle($h)
            if ($root.Current.ProcessId -eq $fmtPid -and [int64]$root.Current.NativeWindowHandle -eq $h.ToInt64()) {
                Get-FmtUiDump $root $walker $limit -Popup -Capture $capture
            }
        } catch { })
        if ($tree.Count) { $tree }
        else { [ordered]@{ Capture = $capture; Depth = 0; Status = 'UI Automation unavailable' } | ConvertTo-Json -Compress }
    })
    if (-not $lines.Count) { throw 'no rekordbox windows remained for the close dump' }
    Write-FmtUiDump $lines
}

function Get-FmtSafeCaption([string]$title) {
    if ($title -cin 'rekordbox', 'menu', 'Open', 'Import Playlist', 'Import Track', 'Save As',
        'Export a playlist to a file for music apps (*.m3u8)', 'Import Error', 'Delete Playlist') { return $title }
    return '<redacted>'
}

function Get-FmtWindowDescription([IntPtr]$h) {
    if ($h -eq [IntPtr]::Zero) { return 'no window handle' }
    $windowPid = [RbspWin]::Pid($h)
    $class = [RbspFormatInput]::WindowClass($h) -replace '[\r\n]+', ' '
    $size = [RbspFormatInput]::WindowSize($h)
    $owner = [RbspWin]::GetWindow($h, 4) # GW_OWNER
    $ownerClass = if ($owner -eq [IntPtr]::Zero) { 'none' } else {
        [RbspFormatInput]::WindowClass($owner) -replace '[\r\n]+', ' '
    }
    $detail = "pid=$windowPid class=$class size=$size handle=0x$($h.ToString('X')) owner-class=$ownerClass owner-pid=$([RbspWin]::Pid($owner))"
    # Even rekordbox can put filenames in its window title.
    if ($windowPid -eq $fmtPid) {
        $title = Get-FmtSafeCaption ([RbspFormatInput]::WindowTitle($h))
        if ($title -cne '<redacted>') { $detail += " caption=$title" }
    }
    return $detail
}

function Stop-FmtWindow([string]$reason, [IntPtr]$h) {
    throw "formats: $reason; window $(Get-FmtWindowDescription $h)"
}

function Stop-FmtFocus([string]$reason, [IntPtr]$foreground = [RbspWin]::GetForegroundWindow()) {
    throw "formats: $reason; foreground $(Get-FmtWindowDescription $foreground)"
}

function Test-FmtTooltip([IntPtr]$h, [string]$line) {
    # The caller has already matched the process and the top-level window list.
    if ($line -cnotmatch ' \| ([1-9][0-9]*)x([1-9][0-9]*) \| 0x[0-9A-F]+$') { return $false }
    $width, $height = [int]$Matches[1], [int]$Matches[2]
    if (-not (($width -le 400 -and $height -le 120) -or $width -le 12 -or $height -le 12) -or
        -not [RbspFormatInput]::WindowClass($h).StartsWith('JUCE_', [StringComparison]::Ordinal)) { return $false }
    $owner = [RbspWin]::GetWindow($h, 4) # GW_OWNER
    return ($owner -eq [IntPtr]::Zero -or [RbspWin]::Pid($owner) -eq $fmtPid) -and
        [RbspWin]::GetForegroundWindow() -ne $h -and [RbspFormatInput]::IsNonActivating($h)
}

function Test-FmtPickerOwner([IntPtr]$h) {
    # JUCE can place a hidden, unowned helper between the native picker and the main window.
    $seen = [Collections.Generic.HashSet[IntPtr]]::new()
    $null = $seen.Add($h)
    $owner = [RbspWin]::GetWindow($h, 4) # GW_OWNER
    if ($owner -eq [IntPtr]::Zero) { return $false }
    do {
        if (-not $seen.Add($owner) -or [RbspWin]::Pid($owner) -ne $fmtPid) { return $false }
        $owner = [RbspWin]::GetWindow($owner, 4)
    } while ($owner -ne [IntPtr]::Zero)
    return $true
}

function Get-FmtPickerTitles([string]$phase) {
    if ($phase -in 'Open', 'ImportResult', 'ExploreOpen', 'ClosePicker') { 'Import Playlist' }
    if ($phase -in 'ExploreOpen', 'ClosePicker') { 'Open'; 'Import Track' }
    if ($phase -in 'ExploreOpen', 'ClosePicker') { 'Save As'; 'Export a playlist to a file for music apps (*.m3u8)' }
}

function Test-FmtNativePicker([IntPtr]$h, [string]$phase) {
    return [RbspWin]::Pid($h) -eq $fmtPid -and [RbspFormatInput]::IsWindowVisible($h) -and
        [RbspFormatInput]::WindowClass($h) -ceq '#32770' -and
        (Get-FmtPickerTitles $phase) -ccontains [RbspFormatInput]::WindowTitle($h) -and (Test-FmtPickerOwner $h)
}

function Test-FmtJucePopupRoot($c) {
    return $c.ControlType -eq [Windows.Automation.ControlType]::Window -and $c.Name -ceq 'rekordbox' -and
        $c.AutomationId -ceq '' -and $c.FrameworkId -ceq 'JUCE' -and $c.ClassName -cmatch '^JUCE_[a-f0-9]+$'
}

function Test-FmtImportProgress($win) {
    if (-not (Test-FmtJucePopupRoot $win.Current)) { return $false }
    $walker = [Windows.Automation.TreeWalker]::ControlViewWalker
    $bounds = $win.Current.BoundingRectangle
    $kinds = @(for ($child = $walker.GetFirstChild($win); $child; $child = $walker.GetNextSibling($child)) {
        $c = $child.Current
        if ($c.ProcessId -ne $fmtPid -or $c.FrameworkId -cne 'JUCE' -or $c.IsOffscreen -or
            -not $bounds.Contains($c.BoundingRectangle)) { return $false }
        $ct = [Windows.Automation.ControlType]
        if ($c.ControlType -eq $ct::Button -and $c.Name -ceq 'Cancel' -and $c.AutomationId -ceq 'Cancel.<empty>') { 'cancel' }
        elseif ($c.ControlType -eq $ct::Text) { 'text' }
        elseif ($c.ControlType -eq $ct::ProgressBar -and $c.AutomationId -ceq '.<empty>') { 'progress' }
        else { return $false }
    })
    return ($kinds.Count -eq 3 -and (($kinds | Sort-Object) -join ',') -ceq 'cancel,progress,text')
}

function Test-FmtJuceMenu($win) {
    $r = $win.Current.BoundingRectangle
    $walker = [Windows.Automation.TreeWalker]::ControlViewWalker
    $known = $false
    $child = $walker.GetFirstChild($win)
    for ($i = 0; $child -and $i -lt 100; $i++) {
        $item = $child.Current
        $b = $item.BoundingRectangle
        if ($item.ProcessId -ne $fmtPid -or $item.ControlType -ne [Windows.Automation.ControlType]::MenuItem -or $item.FrameworkId -cne 'JUCE' -or
            $b.Left -ne $r.Left -or $b.Width -ne $r.Width -or -not $r.Contains($b)) { return $false }
        if ((Get-FmtMenuCaption $item.Name) -cin 'Import', 'Import Playlist', 'Import Track', 'Export Playlist',
            'Export a playlist to a file', 'Export a playlist to a file for music apps (*.m3u8)', 'Delete Playlist') { $known = $true }
        $child = $walker.GetNextSibling($child)
    }
    return $known -and -not $child
}

function Test-FmtMenuParent($parent, $menus) {
    $p = $parent.Current.BoundingRectangle
    foreach ($menu in $menus) {
        $c = $menu.Current
        $r = $c.BoundingRectangle
        if ($parent.Current.ClassName -ceq $c.ClassName -and ($p.Right -eq $r.Left -or $p.Left -eq $r.Right) -and
            $r.Top -ge $p.Top -and $r.Top -lt $p.Bottom) { return $true }
    }
    return $false
}

function Test-FmtMenuShadow($shadow, $menus) {
    $s = $shadow.BoundingRectangle
    foreach ($menu in $menus) {
        $c = $menu.Current
        if ($shadow.ClassName -cne $c.ClassName) { continue }
        $r = $c.BoundingRectangle
        if ($s.Height -gt 0 -and $s.Height -le 28 -and $s.Left -eq $r.Left -and $s.Right -eq $r.Right -and
            ($s.Bottom -eq $r.Top -or $s.Top -eq $r.Bottom)) { return $true }
        if ($s.Width -gt 0 -and $s.Width -le 28 -and $s.Top -eq $r.Top - $s.Width -and $s.Bottom -eq $r.Bottom + $s.Width -and
            ($s.Right -eq $r.Left -or $s.Left -eq $r.Right)) { return $true }
    }
    return $false
}

function Get-FmtWindowState([string]$phase, [IntPtr]$foreground = [IntPtr]::Zero) {
    if ((Get-Date) -ge $fmtDeadline -or ($fmtStop -and (Test-Path -LiteralPath $fmtStop))) { throw 'formats: desktop task stopped or timed out' }
    $windows = @(Get-DesktopWindows -AllSizes)
    if (-not [RbspWin]::OrdinaryDesktop()) { Stop-FmtFocus 'ordinary input desktop unavailable' }
    # A native dialog can take focus during enumeration, before its HWND appears in the list.
    if ($foreground -ne $fmtHandle -and (Test-FmtNativePicker $foreground $phase)) {
        $suffix = ' | 0x' + $foreground.ToString('X')
        if (-not ($windows | Where-Object { $_.EndsWith($suffix, [StringComparison]::Ordinal) })) {
            $title = [RbspFormatInput]::WindowTitle($foreground)
            $windows += "rekordbox | $title | $([RbspFormatInput]::WindowSize($foreground))$suffix"
        }
    }
    $mainLine = "rekordbox | rekordbox | "
    $mainHandle = ' | 0x' + $fmtHandle.ToString('X')
    if (-not @($windows | Where-Object { $_.StartsWith($mainLine) -and $_.EndsWith($mainHandle) }).Count -or
        [RbspWin]::Pid($fmtHandle) -ne $fmtPid) { Stop-FmtWindow 'rekordbox main window changed' $fmtHandle }

    $allowed = @($fmtHandle)
    $dialogs = @()
    $tooltips = 0
    $menuPhase = $phase -in 'Menu', 'ExploreOpen'
    $menuWindows = @()
    $menuParents = @()
    $shadows = @()
    $progress = @()
    foreach ($line in $windows) {
        if ($line -match '^(Upmgr rekordbox|rekordboxAgent|WerFault(Secure)?|smartscreen|consent|CredentialUIBroker|SecurityHealthHost|AppInstaller) \|' -or
            $line -match ' \| (rekordbox update manager|Windows Security|User Account Control|.* - Security Warning) \|') {
            Stop-FmtWindow 'unexpected update, security or application window' (Get-FmtHandle $line)
        }
        if (-not $line.StartsWith('rekordbox | ') -or $line.EndsWith($mainHandle)) { continue }
        $h = Get-FmtHandle $line
        if (-not [RbspFormatInput]::IsWindowVisible($h)) { continue }
        if ([RbspWin]::Pid($h) -ne $fmtPid) { Stop-FmtWindow 'another rekordbox process has a window' $h }
        $menuCaption = $menuPhase -and $line -cmatch '^rekordbox \| (?:menu|rekordbox)? \| '
        $tooltip = Test-FmtTooltip $h $line
        if ($tooltip -and -not $menuCaption) { $tooltips++; continue }
        try {
            $win = [Windows.Automation.AutomationElement]::FromHandle($h)
            $c = $win.Current
        } catch {
            if (-not [RbspFormatInput]::IsWindowVisible($h)) { continue }
            Stop-FmtWindow 'could not inspect rekordbox window' $h
        }
        if ($c.ProcessId -ne $fmtPid -or [int64]$c.NativeWindowHandle -ne $h.ToInt64()) {
            Stop-FmtWindow 'unexpected rekordbox window identity' $h
        }

        # The native file picker can have a JUCE helper owner reported as
        # visible by Win32 even though it has no UIA bounds. It is no input target.
        if ($c.ControlType -eq [Windows.Automation.ControlType]::Custom -and
            $c.FrameworkId -ceq 'JUCE' -and [RbspFormatInput]::WindowClass($h) -cmatch '^JUCE_[a-f0-9]+$' -and
            (($c.IsOffscreen -and $c.BoundingRectangle.IsEmpty) -or $line -cmatch ' \| (?:0x[0-9]+|[0-9]+x0) \| 0x[0-9A-F]+$')) {
            continue
        }

        if ($phase -eq 'ImportResult' -and [RbspWin]::GetWindow($h, 4) -eq [IntPtr]::Zero -and
            (Test-FmtImportProgress $win)) {
            $progress += $win
            continue
        }
        if ($menuCaption -and (Test-FmtJucePopupRoot $c)) {
            if (Test-FmtJuceMenu $win) {
                $allowed += $h
                $menuWindows += $win
                if ($phase -eq 'Menu') { $dialogs += $win }
                continue
            }
            # JUCE hides the parent menu's UIA rows while its submenu is open.
            if (-not [Windows.Automation.TreeWalker]::ControlViewWalker.GetFirstChild($win)) {
                $menuParents += $win
                continue
            }
        }
        # JUCE's menu popups use the name "menu". They are scoped by process and only allowed
        # after opening a menu. Native menus can have an empty Win32 caption.
        if ($menuCaption -and
            $c.Name -cin '', 'menu' -and $c.ControlType -eq [Windows.Automation.ControlType]::Menu -and
            ($c.FrameworkId -ceq 'JUCE' -or $c.ClassName -ceq '#32768')) {
            $allowed += $h
            if ($phase -eq 'Menu') { $dialogs += $win }
            continue
        }
        if ($tooltip) { $tooltips++; continue }
        if (($menuPhase -or $phase -eq 'ImportResult') -and $c.Name -ceq '' -and $c.FrameworkId -ceq 'Win32' -and
            $c.ControlType -eq [Windows.Automation.ControlType]::Pane -and $c.ClassName -cmatch '^JUCE_[a-f0-9]+$' -and
            [RbspWin]::GetForegroundWindow() -ne $h) {
            $shadows += $c
            continue
        }
        $picker = (Get-FmtPickerTitles $phase) -ccontains $c.Name
        $owned = if ($picker) { Test-FmtPickerOwner $h } else { [RbspWin]::GetWindow($h, 4) -eq $fmtHandle }
        if (-not $owned) { Stop-FmtWindow 'unexpected rekordbox window owner' $h }
        $titles = switch ($phase) {
            { $_ -in 'Refusal', 'ImportResult' } { 'rekordbox'; 'Import Error' }
        }
        if ((-not $picker -and $titles -cnotcontains $c.Name) -or -not $line.StartsWith("rekordbox | $($c.Name) | ", [StringComparison]::Ordinal) -or
            $c.ControlType -ne [Windows.Automation.ControlType]::Window -or
            ($picker -and ($c.ClassName -cne '#32770' -or -not (Test-FmtNativePicker $h $phase) -or
                           [RbspFormatInput]::WindowTitle($h) -cne $c.Name))) {
            Stop-FmtWindow 'unexpected rekordbox window' $h
        }
        $allowed += $h
        $dialogs += $win
    }
    foreach ($parent in $menuParents) {
        if (-not (Test-FmtMenuParent $parent $menuWindows)) {
            Stop-FmtWindow 'unexpected empty rekordbox menu window' ([IntPtr]$parent.Current.NativeWindowHandle)
        }
        $allowed += [IntPtr]$parent.Current.NativeWindowHandle
        if ($phase -eq 'Menu') { $dialogs += $parent }
    }
    $menuWindows += $menuParents
    foreach ($shadow in $shadows) {
        if (-not (Test-FmtMenuShadow $shadow ($menuWindows + $progress))) {
            Stop-FmtWindow 'unexpected rekordbox window beside a menu' ([IntPtr]$shadow.NativeWindowHandle)
        }
    }
    if ($phase -ne 'Menu' -and $dialogs.Count -gt 1) {
        $details = foreach ($dialog in $dialogs) { Get-FmtWindowDescription ([IntPtr]$dialog.Current.NativeWindowHandle) }
        throw "formats: multiple rekordbox dialogs; $($details -join '; ')"
    }
    return @{ Handles = $allowed; Dialogs = $dialogs; Tooltips = $tooltips; ImportProgress = $progress }
}

function Set-FmtForeground([IntPtr]$target, [IntPtr]$foreground) {
    $ownerPid = [uint32]0
    $foregroundThread = [RbspWin]::GetWindowThreadProcessId($foreground, [ref]$ownerPid)
    $currentThread = [RbspFormatInput]::GetCurrentThreadId()
    $attached = $false
    try {
        if ($foregroundThread -and $foregroundThread -ne $currentThread) {
            $attached = [RbspFormatInput]::AttachThreadInput($currentThread, $foregroundThread, $true)
            if (-not $attached) { return }
        }
        if ([RbspWin]::GetForegroundWindow() -ne $foreground -or [RbspWin]::Pid($target) -ne $fmtPid) { return }
        [void][RbspFormatInput]::SetForegroundWindow($target)
        [void][RbspFormatInput]::BringWindowToTop($target)
    } finally {
        if ($attached) { [void][RbspFormatInput]::AttachThreadInput($currentThread, $foregroundThread, $false) }
    }
}

function Get-FmtWindows([string]$phase, [switch]$Refocus, [switch]$Dialog) {
    $until = (Get-Date).AddSeconds(10)
    $tooltipUntil = $null
    $pickerTitles = @(Get-FmtPickerTitles $phase)
    do {
        # Recheck window identities and blockers on every poll before requesting foreground.
        $before = [RbspWin]::GetForegroundWindow()
        $state = Get-FmtWindowState $phase $before
        $foreground = [RbspWin]::GetForegroundWindow()
        if ($state.Handles -notcontains $foreground -and $foreground -ne $before -and
            (Get-Date) -lt $until -and (Test-FmtNativePicker $foreground $phase)) { continue }
        $target = $fmtHandle
        $valid = $state.Handles -contains $foreground
        if ($Dialog -and $state.Dialogs.Count) {
            $target = [IntPtr]$state.Dialogs[0].Current.NativeWindowHandle
            $valid = $foreground.ToInt64() -eq $target.ToInt64()
        }
        if ($valid -and (-not $Refocus -or -not [RbspWin]::IsIconic($fmtHandle))) {
            if ($state.Tooltips -and $phase -in 'Main', 'Menu', 'ExploreOpen') {
                if (-not $tooltipUntil) { $tooltipUntil = (Get-Date).AddMilliseconds(200) }
                if ((Get-Date) -lt $tooltipUntil) { Start-Sleep -Milliseconds 100; continue }
            }
            return $state.Dialogs
        }
        $focusIssue = 'focus left the verified rekordbox windows'
        if ($pickerTitles.Count -and [RbspWin]::Pid($foreground) -eq $fmtPid -and
            [RbspFormatInput]::WindowClass($foreground) -ceq '#32770') {
            $expectedTitle = $pickerTitles -ccontains [RbspFormatInput]::WindowTitle($foreground)
            $nativePicker = Test-FmtNativePicker $foreground $phase
            # A same-process native picker can activate before its owner and UIA surface settle.
            # Poll without sending input until it passes the full window checks.
            if ($expectedTitle -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 100; continue }
            $focusIssue += " (phase=$phase target=0x$($target.ToString('X')) dialogs=$($state.Dialogs.Count) expected-title=$expectedTitle native-picker=$nativePicker)"
        }
        if (-not $Refocus) { Stop-FmtFocus $focusIssue $foreground }
        $app = $false
        $explorerWindow = $false
        if ($state.Handles -notcontains $foreground -and $foreground -ne [IntPtr]::Zero) {
            $process = Get-Process -Id ([RbspWin]::Pid($foreground)) -ErrorAction SilentlyContinue
            $shell = $process.ProcessName -ieq 'explorer' -and
                [RbspFormatInput]::WindowClass($foreground) -cin 'Progman', 'WorkerW', 'Shell_TrayWnd', 'Shell_SecondaryTrayWnd'
            $explorerWindow = $phase -ceq 'Main' -and $process.ProcessName -ieq 'explorer' -and
                $process.SessionId -eq $sid -and $process.Path -ieq (Join-Path $env:SystemRoot 'explorer.exe') -and
                [RbspFormatInput]::WindowClass($foreground) -ceq 'CabinetWClass' -and
                [RbspFormatInput]::IsWindowVisible($foreground) -and [RbspWin]::GetWindow($foreground, 4) -eq [IntPtr]::Zero
            $app = $process.ProcessName -ceq 'RB Stems Plus' -and $process.SessionId -eq $sid -and
                $process.MainWindowHandle -eq $foreground -and [RbspWin]::GetWindow($foreground, 4) -eq [IntPtr]::Zero -and
                [RbspFormatInput]::WindowTitle($foreground) -ceq 'RB Stems Plus'
            # the start step's closeagentnews only hides rekordboxAgent's news window, which can keep
            # the foreground while invisible (PC 2026-11-21): tolerate it and bring rekordbox forward
            $hiddenAgent = $process.ProcessName -ceq 'rekordboxAgent' -and $process.SessionId -eq $sid -and
                -not [RbspFormatInput]::IsWindowVisible($foreground)
            if (-not ($shell -or $app -or $hiddenAgent -or $explorerWindow)) { Stop-FmtFocus $focusIssue $foreground }
        }
        if ((Get-Date) -ge $until) { Stop-FmtFocus 'timed out waiting for verified rekordbox foreground' $foreground }
        if (-not [RbspWin]::OrdinaryDesktop()) { Stop-FmtFocus 'ordinary input desktop unavailable' }
        if ([RbspWin]::Pid($target) -ne $fmtPid) { Stop-FmtWindow 'rekordbox window changed before foreground request' $target }
        if ([RbspWin]::GetForegroundWindow() -eq $foreground) {
            if ([RbspWin]::IsIconic($fmtHandle)) { [void][RbspFormatInput]::ShowWindowAsync($fmtHandle, 9) } # SW_RESTORE
            elseif (($app -or $explorerWindow) -and $phase -ceq 'Main') {
                # Use the MIDI route's verified UI Automation handoff before sending import input.
                if ((Set-RbFront) -cnotlike 'front: ok; *') { Stop-FmtFocus 'could not focus rekordbox through UI Automation' }
            }
            elseif ($foreground -eq [IntPtr]::Zero -or
                    ([RbspFormatInput]::WindowClass($foreground) -cin 'Progman', 'WorkerW', 'Shell_TrayWnd', 'Shell_SecondaryTrayWnd' -and
                     (Get-Process -Id ([RbspWin]::Pid($foreground)) -ErrorAction SilentlyContinue).ProcessName -ieq 'explorer') -or
                    -not [RbspFormatInput]::IsWindowVisible($foreground)) {
                Set-FmtForeground $target $foreground
            }
            else { [void][RbspFormatInput]::SetForegroundWindow($target) }
        }
        Start-Sleep -Milliseconds 100
    } while ($true)
}

function Find-FmtControls($root, $type, [string[]]$names, [switch]$Hidden, [string]$Id) {
    $A = [Windows.Automation.AutomationElement]
    $condition = [Windows.Automation.PropertyCondition]::new($A::ControlTypeProperty, $type)
    foreach ($el in $root.FindAll([Windows.Automation.TreeScope]::Descendants, $condition)) {
        $c = $el.Current
        if ($c.ProcessId -eq $fmtPid -and ($Hidden -or -not $c.IsOffscreen) -and
            (-not $names -or [array]::IndexOf($names, [string]$c.Name) -ge 0) -and
            (-not $Id -or $c.AutomationId -ceq $Id)) { $el }
    }
}

function Cancel-FmtPickerForClose {
    $dialog = Get-FmtWindows 'ClosePicker' -Refocus -Dialog
    if (-not $dialog) { return 'no picker' }
    Invoke-FmtPickerButton $dialog 'ClosePicker' 'Cancel' '2'
    $end = (Get-Date).AddSeconds(5)
    do {
        if (-not (Get-FmtWindowState 'ClosePicker').Dialogs.Count) { return 'cancelled' }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $end)
    throw 'formats: the verified picker did not close after Cancel'
}

function Get-FmtControl($root, $type, [string[]]$names, [string]$Id, [switch]$Popup) {
    $found = @(Find-FmtControls $root $type $names -Id $Id)
    if ($found.Count -ne 1 -or -not $found[0].Current.IsEnabled) {
        $candidates = $found
        if (-not $candidates.Count) { $candidates = @(Find-FmtControls $root $type $names -Id $Id -Hidden) }
        if (-not $candidates.Count) { $candidates = @(Find-FmtControls $root $type @() -Hidden) }
        Stop-FmtControls "expected one enabled UI control ($($type.ProgrammaticName), matches=$($found.Count))" $candidates -Popup:$Popup
    }
    return $found[0]
}

function Stop-FmtControls([string]$reason, [object[]]$candidates, [switch]$Popup) {
    $walker = [Windows.Automation.TreeWalker]::ControlViewWalker
    $details = @(foreach ($el in ($candidates | Select-Object -First 10)) {
        $c = $el.Current
        $privateNames = @()
        $name = '<redacted>'
        for ($i = 0; $i -le 12 -and $el -and $el.Current.ProcessId -eq $fmtPid; $i++) {
            $current = $el.Current
            $position = if ($current.ControlType.ProgrammaticName -ceq 'ControlType.TreeItem') { Get-FmtTreePosition $el $walker }
            $level = if ($position) { $position.Level } else { -1 }
            $safe = Get-FmtUiName $current $level -Popup:$Popup
            if ($i -eq 0) { $name = $safe }
            if ($safe -ceq '<redacted>' -and $current.Name) { $privateNames += $current.Name }
            if ($current.ControlType.ProgrammaticName -ceq 'ControlType.Window') { break }
            $el = $walker.GetParent($el)
        }
        Get-FmtUiIdentity $c $name $privateNames | ConvertTo-Json -Compress
    })
    throw "formats: $reason; candidates=$($candidates.Count) [$($details -join ',')]"
}

function Move-FmtCursor {
    if ([RbspWin]::OrdinaryDesktop() -and [RbspWin]::Pid($fmtHandle) -eq $fmtPid) {
        [void][RbspFormatInput]::ParkCursor($fmtHandle)
    }
}

function Invoke-FmtMouseClick([IntPtr]$expected, [IntPtr]$target, [int]$x, [int]$y, [bool]$right, [ref]$foreground) {
    if (-not [RbspFormatInput]::Click($expected, $target, $x, $y, $right, $foreground)) { return $false }
    Move-FmtCursor
    return $true
}

function Invoke-FmtClick($el, [switch]$Right, [IntPtr]$popup = [IntPtr]::Zero, $Point, [switch]$Hover) {
    $phase = if ($popup -ne [IntPtr]::Zero) { 'Menu' } else { 'Main' }
    $popups = @(Get-FmtWindows $phase)
    if ($phase -eq 'Menu' -and $popups.Current.NativeWindowHandle -notcontains $popup) {
        throw 'formats: the menu popup changed before the click'
    }
    $target = if ($popup -ne [IntPtr]::Zero) { $popup } else { $fmtHandle }
    $c = $el.Current
    if ($c.ProcessId -ne $fmtPid -or $c.IsOffscreen -or -not $c.IsEnabled) {
        throw 'formats: the rekordbox control cannot be clicked'
    }
    if (-not $PSBoundParameters.ContainsKey('Point')) {
        $Point = New-Object Windows.Point
        if (-not $el.TryGetClickablePoint([ref]$Point)) {
            $r = $c.BoundingRectangle
            $Point = [Windows.Point]::new($r.Left + $r.Width / 2, $r.Top + $r.Height / 2)
        }
    }
    $Point = [Windows.Point]::new([int]$Point.X, [int]$Point.Y)
    if (-not $c.BoundingRectangle.Contains($Point)) { throw 'formats: the click is outside the verified control' }
    $atPoint = [Windows.Automation.AutomationElement]::FromPoint($point)
    $hit = $atPoint
    while ($hit -and -not [Windows.Automation.Automation]::Compare($hit, $el)) {
        $hit = [Windows.Automation.TreeWalker]::RawViewWalker.GetParent($hit)
    }
    if (-not $hit -and $atPoint -and $atPoint.Current.ProcessId -eq $fmtPid) {
        # JUCE (rekordbox) answers hit tests with the control's container, not the control itself
        # (PC 2026-11-21, the source icon column). The point is already inside the control's own
        # rectangle; accept a hit on one of its own ancestors (at most 3 levels up), so another
        # control or window over the point still stops the click.
        $up = $el
        for ($i = 0; $i -lt 3 -and -not $hit; $i++) {
            $up = [Windows.Automation.TreeWalker]::RawViewWalker.GetParent($up)
            if (-not $up) { break }
            if ([Windows.Automation.Automation]::Compare($up, $atPoint)) { $hit = $el }
        }
    }
    if (-not $hit) {
        $null = Get-FmtWindows $phase
        throw 'formats: the rekordbox control is obscured'
    }
    $expected = [RbspWin]::GetForegroundWindow()
    if ($expected -ne $fmtHandle -and $popups.Current.NativeWindowHandle -notcontains $expected) {
        Stop-FmtFocus 'focus left the verified click target' $expected
    }
    $foreground = [IntPtr]::Zero
    $positioned = if ($Hover) {
        [RbspFormatInput]::PointAt($expected, $target, [int]$point.X, [int]$point.Y, [ref]$foreground)
    } else {
        Invoke-FmtMouseClick $expected $target ([int]$point.X) ([int]$point.Y) $Right ([ref]$foreground)
    }
    if (-not $positioned) {
        if ($foreground -ne $expected) { Stop-FmtFocus 'focus left the verified rekordbox windows before the click' $foreground }
        throw 'formats: the rekordbox control is obscured'
    }
}

function Find-FmtMenuEntries([string[]]$names, [object[]]$roots, [switch]$Bar) {
    foreach ($root in $roots) {
        foreach ($el in (Find-FmtControls $root ([Windows.Automation.ControlType]::MenuItem) @())) {
            if ([array]::IndexOf($names, (Get-FmtMenuCaption $el.Current.Name)) -lt 0) { continue }
            @{ El = $el; Popup = $(if ($Bar) { [IntPtr]::Zero } else { [IntPtr]$root.Current.NativeWindowHandle }) }
        }
    }
}

function Send-FmtKeys([string]$phase, [string]$keys) {
    $popups = @(Get-FmtWindows $phase)
    $foreground = [RbspWin]::GetForegroundWindow()
    if ($foreground -ne $fmtHandle -and $popups.Current.NativeWindowHandle -notcontains $foreground) {
        Stop-FmtFocus 'focus left the verified keyboard target' $foreground
    }
    [Windows.Forms.SendKeys]::SendWait($keys)
}

function Open-FmtFileMenuBar {
    $null = Get-FmtWindows 'Main'
    $x = $y = 0
    $status = [RbspFormatInput]::FileMenuPoint($fmtHandle, [ref]$x, [ref]$y)
    if (-not $status) { return $false }
    if ($status -lt 0) { throw 'formats: the native File menu is ambiguous or disabled' }
    $foreground = [IntPtr]::Zero
    if (-not (Invoke-FmtMouseClick $fmtHandle $fmtHandle $x $y $false ([ref]$foreground))) {
        if ($foreground -ne $fmtHandle) { Stop-FmtFocus 'focus left the verified File menu' $foreground }
        throw 'formats: the native File menu is obscured'
    }
    return $true
}

function Open-FmtVisibleFileMenu {
    $null = Get-FmtWindows 'Main'
    $window = $fmtMain.Current.BoundingRectangle
    $unit = (Get-FmtSourceLayout).Unit
    $barHeight = 22 * $unit
    $content = @(Find-FmtControls $fmtMain ([Windows.Automation.ControlType]::Custom) @() | Where-Object {
        $r = $_.Current.BoundingRectangle
        $_.Current.FrameworkId -ceq 'JUCE' -and $r.Left -eq $window.Left -and $r.Right -eq $window.Right -and
            $r.Bottom -eq $window.Bottom -and [math]::Abs($r.Top - ($window.Top + $barHeight)) -le $unit
    })
    if ($content.Count -ne 1) { throw 'formats: could not verify the visible menu bar layout' }
    $modeRect = [Windows.Rect]::new(($window.Left + 10 * $unit), ($content[0].Current.BoundingRectangle.Top + 6 * $unit),
        (92 * $unit), (16 * $unit))
    $mode = Read-FmtClientText $modeRect 4
    if ($mode -cin 'PERFORMANCE', 'EXPORT') { $fmtStep.Mode = $mode }
    $fileRect = [Windows.Rect]::new(($window.Left + 4 * $unit), ($window.Top + $unit), (35 * $unit), (20 * $unit))
    if ((Read-FmtClientText $fileRect 4) -cne 'File') { throw 'formats: could not read File in the verified menu bar' }
    $null = Get-FmtWindows 'Main'
    $foreground = [IntPtr]::Zero
    if (-not [RbspFormatInput]::Click($fmtHandle, $fmtHandle, [int]($fileRect.Left + $fileRect.Width / 2),
            [int]($fileRect.Top + $fileRect.Height / 2), $false, [ref]$foreground)) {
        if ($foreground -ne $fmtHandle) { Stop-FmtFocus 'focus left the verified File menu' $foreground }
        throw 'formats: the File menu is obscured'
    }
}

function Close-FmtMenu {
    foreach ($attempt in 1..2) {
        $popups = @(Get-FmtWindows 'Menu')
        if (-not $popups.Count) { return }
        Send-FmtKeys 'Menu' '{ESC}'
        Start-Sleep -Milliseconds 200
    }
    $null = Get-FmtWindows 'Main'
}

function Open-FmtExploreMenu([string]$name, [object[]]$within, [switch]$NoShortcut) {
    # Import and Export parents may only expand or receive a pointer move. The
    # documented Import Playlist leaf only opens its file picker.
    if ($name -cnotin 'File', 'Import', 'Import Playlist', 'Export a playlist to a file', 'Export Playlist', 'Export') {
        throw 'formats: menu action is outside the explore allowlist'
    }
    $bar = $name -ceq 'File'
    $phase = if ($bar) { 'Main' } else { 'Menu' }
    $popups = @(Get-FmtWindows $phase)
    $roots = if ($bar) { $fmtMain } else {
        foreach ($root in $within) {
            if (-not ($popups | Where-Object { [Windows.Automation.Automation]::Compare($_, $root) })) {
                throw 'formats: the captured explore menu changed'
            }
            $root
        }
    }
    $items = @(Find-FmtMenuEntries $name $roots -Bar:$bar)
    if ($bar -and -not $items.Count) {
        if (-not (Open-FmtFileMenuBar)) {
            if ($NoShortcut) { Open-FmtVisibleFileMenu }
            else { Send-FmtKeys 'Main' '%f' }
        }
        return
    }
    if ($items.Count -ne 1 -or -not $items[0].El.Current.IsEnabled) {
        Stop-FmtControls "expected one enabled $name menu item" $items.El -Popup
    }
    $item = $items[0]
    $again = @(Get-FmtWindows $phase)
    if ((-not $bar -and $again.Current.NativeWindowHandle -notcontains $item.Popup) -or
        (Get-FmtMenuCaption $item.El.Current.Name) -cne $name) { throw 'formats: the explore menu item changed' }
    $expand = $null
    $canExpand = $item.El.TryGetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$expand)
    if ($canExpand -and $expand.Current.ExpandCollapseState -eq 'Collapsed' -and $name -cne 'Import Playlist') {
        $expand.Expand()
    } elseif ($name -cin 'File', 'Import Playlist') {
        if (-not (Invoke-El $item.El)) { Invoke-FmtClick $item.El -Popup $item.Popup }
    } else {
        Invoke-FmtClick $item.El -Popup $item.Popup -Hover
    }
}

function Get-FmtExploreMenuSurfaces($popups) {
    foreach ($popup in $popups) {
        $popup
        Find-FmtControls $popup ([Windows.Automation.ControlType]::Menu) @()
    }
}

function Wait-FmtExploreMenu([object[]]$previous, [switch]$Optional) {
    $end = (Get-Date).AddSeconds(3)
    do {
        $popups = @(Get-FmtWindows 'Menu')
        foreach ($surface in (Get-FmtExploreMenuSurfaces $popups)) {
            $new = $true
            foreach ($old in $previous) {
                if ([Windows.Automation.Automation]::Compare($old, $surface)) { $new = $false; break }
            }
            if ($new) { return $popups }
        }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $end)
    if (-not $Optional -or -not $popups.Count) { throw 'formats: the expected explore menu did not open' }
}

function Save-FmtExploreCapture([string]$label, [object[]]$roots, [string]$result = 'opened') {
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add(([ordered]@{ Capture = $label; Result = $result } | ConvertTo-Json -Compress))
    $walker = [Windows.Automation.TreeWalker]::ControlViewWalker
    foreach ($root in $roots) {
        if ($lines.Count -ge 500) { break }
        $lines.AddRange([string[]]@(Get-FmtUiDump $root $walker (500 - $lines.Count) -Popup -Capture $label))
    }
    if ($fmtExplore.File) {
        $path = Join-Path $B $fmtExplore.File
        Assert-FormatPath $path
        Add-Content -LiteralPath $path -Value $lines.ToArray() -Encoding UTF8
    } else {
        $fmtExplore.File = Write-FmtUiDump $lines.ToArray()
    }
    $fmtExplore.Count += $lines.Count
}

function Read-FmtExploreDialog([string]$label, [switch]$Optional) {
    $end = (Get-Date).AddSeconds(5)
    do {
        $dialog = Get-FmtWindows 'ExploreOpen' -Dialog
        if ($dialog) { break }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $end)
    if (-not $dialog) {
        Save-FmtExploreCapture $label @() 'no-dialog'
        if (-not $Optional) { throw 'formats: the playlist file dialog did not open' }
        return
    }
    $standard = $dialog.Current.ClassName -ceq '#32770'
    $result = if ($standard -and $dialog.Current.Name -ceq 'Open') { 'standard-open' } else { 'other-dialog' }
    Save-FmtExploreCapture $label @($dialog) $result
    if (-not $standard -and $dialog.Current.Name -cnotin 'Open', 'Import Playlist', 'Import Track') {
        throw 'formats: unexpected title on the explore dialog'
    }
    $id = if ($standard) { '2' }
    $cancel = Get-FmtControl $dialog ([Windows.Automation.ControlType]::Button) 'Cancel' $id -Popup
    $again = Get-FmtWindows 'ExploreOpen' -Dialog
    if (-not $again -or -not [Windows.Automation.Automation]::Compare($again, $dialog) -or
        -not (Invoke-El $cancel)) { throw 'formats: could not cancel the verified explore dialog' }
    $end = (Get-Date).AddSeconds(3)
    do {
        $again = Get-FmtWindows 'ExploreOpen' -Dialog
        if (-not $again) { Close-FmtMenu; return }
        if (-not [Windows.Automation.Automation]::Compare($again, $dialog)) {
            throw 'formats: a different dialog appeared after Cancel'
        }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $end)
    throw 'formats: the explore dialog remained open after Cancel'
}

function Invoke-FmtExplore {
    $fmtProgress.Step = 'file-menu'
    Open-FmtExploreMenu 'File'
    $fileMenu = @(Wait-FmtExploreMenu)
    Save-FmtExploreCapture 'file-menu' $fileMenu

    $fmtProgress.Step = 'import-menu'
    $previous = @(Get-FmtExploreMenuSurfaces $fileMenu)
    Open-FmtExploreMenu 'Import' $fileMenu
    $importMenu = @(Wait-FmtExploreMenu $previous)
    Save-FmtExploreCapture 'import-menu' $importMenu
    $track = @(Find-FmtMenuEntries 'Import Track' $importMenu)
    # The Performance 1 preset can bind this chord to a deck action instead.
    $ctrlO = $track.Count -eq 1 -and $track[0].El.Current.IsEnabled -and
        ($track[0].El.Current.AcceleratorKey -match '^(?:Ctrl|Control)\+O$' -or
         $track[0].El.Current.Name -match '\t(?:Ctrl|Control)\+O$')

    $fmtProgress.Step = 'import-playlist-dialog'
    Open-FmtExploreMenu 'Import Playlist' $importMenu
    Read-FmtExploreDialog 'import-playlist-dialog'
    $fmtProgress.Step = 'ctrl-o-dialog'
    if ($ctrlO) {
        Send-FmtKeys 'Main' '^o'
        Read-FmtExploreDialog 'ctrl-o-dialog' -Optional
    } else {
        Save-FmtExploreCapture 'ctrl-o-dialog' @() 'binding-unverified'
    }

    $fmtProgress.Step = 'playlist-context-menu'
    $null = Get-FmtWindows 'Main'
    $layout = Get-FmtSourceLayout
    $pane = Open-FmtPlaylistsSource $layout
    $row = Get-FmtExplorePlaylist $layout $pane
    Invoke-FmtClick $row
    Start-Sleep -Milliseconds 300
    $null = Get-FmtWindows 'Main'
    $current = Get-FmtExplorePlaylist $layout (Find-FmtSource $layout 'Playlists')
    if (-not [Windows.Automation.Automation]::Compare($row, $current)) { throw 'formats: the RBSP Tests row changed' }
    Invoke-FmtClick $current -Right
    $context = @(Wait-FmtExploreMenu)
    Save-FmtExploreCapture 'playlist-context-menu' $context

    $fmtProgress.Step = 'playlist-export-menu'
    $export = @(Find-FmtMenuEntries @('Export a playlist to a file', 'Export Playlist', 'Export') $context |
        Where-Object { $_.El.Current.IsEnabled })
    $names = @($export | ForEach-Object { Get-FmtMenuCaption $_.El.Current.Name })
    $submenu = @()
    foreach ($name in $names) {
        $previous = @(Get-FmtExploreMenuSurfaces $context)
        Open-FmtExploreMenu $name $context
        $submenu = @(Wait-FmtExploreMenu $previous -Optional)
        if ($submenu.Count) {
            Save-FmtExploreCapture 'playlist-export-menu' $submenu
            break
        }
    }
    if (-not $submenu.Count) { Save-FmtExploreCapture 'playlist-export-menu' @() 'no-submenu' }
    Close-FmtMenu
    return "fmtuidump: OK $($fmtExplore.Count) $($fmtExplore.File)"
}

function Invoke-FmtStep([string]$action) {
    $keys = @{ 'alt+f' = '%f'; down = '{DOWN}'; right = '{RIGHT}'; up = '{UP}'; left = '{LEFT}' }
    switch -CaseSensitive ($action) {
        'focus' { $null = Get-FmtWindows 'Main' -Refocus }
        'dump' { $null = Get-FmtWindows 'ExploreOpen' }
        'escape' { Send-FmtKeys 'Menu' '{ESC}' }
        'click playlists' {
            $null = Get-FmtWindows 'Main'
            $layout = Get-FmtSourceLayout
            Invoke-FmtClick $layout.Buttons[1]
        }
        'click file' { Open-FmtExploreMenu 'File' -NoShortcut }
        'click import' { Open-FmtExploreMenu 'Import' @(Get-FmtWindows 'Menu') }
        'click import-playlist' { Open-FmtExploreMenu 'Import Playlist' @(Get-FmtWindows 'Menu') }
        'cancel-dialog' {
            $dialog = Get-FmtWindows 'ExploreOpen' -Dialog
            if (-not $dialog) {
                Stop-FmtFocus 'no verified file dialog to cancel'
            }
            $handle = [IntPtr]$dialog.Current.NativeWindowHandle
            if (-not (Test-FmtPickerPath $dialog 'ExploreOpen' '')) {
                Stop-FmtWindow 'could not verify an empty filename before Cancel' $handle
            }
            Invoke-FmtPickerButton $dialog 'ExploreOpen' 'Cancel' '2'
            $end = (Get-Date).AddSeconds(5)
            while ([RbspFormatInput]::IsWindowVisible($handle) -and (Get-Date) -lt $end) {
                $null = Get-FmtWindowState 'ExploreOpen'
                Start-Sleep -Milliseconds 100
            }
            $null = Get-FmtWindows 'Main' -Refocus
        }
        'key alt+f' { Send-FmtKeys 'Main' $keys['alt+f'] }
        { $_ -cmatch '^key (down|right|up|left)$' } {
            $key = $keys[$Matches[1]]
            if (-not @(Get-FmtWindows 'Menu').Count) { throw 'formats: no verified menu for navigation' }
            Send-FmtKeys 'Menu' $key
        }
        default { throw 'formats: unknown step action' }
    }
    if ($action -cne 'dump') { Start-Sleep -Milliseconds 400 }
    $capture = Save-FmtStopDump -Step
    if (-not $capture) { throw 'formats: could not capture the step result' }
    return "fmtstep: OK $capture"
}

function Wait-FmtWindow([string]$phase) {
    $end = (Get-Date).AddSeconds(10)
    do {
        $dlg = Get-FmtWindows $phase -Refocus -Dialog
        if ($dlg) { return $dlg }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $end)
    Stop-FmtFocus "the $phase file dialog did not open"
}

function Set-FmtValue($el, [string]$value) {
    $pattern = $null
    if (-not $el.TryGetCurrentPattern([Windows.Automation.ValuePattern]::Pattern, [ref]$pattern) -or $pattern.Current.IsReadOnly) {
        throw 'formats: the expected text field is read-only'
    }
    $pattern.SetValue($value)
    if ($pattern.Current.Value -cne $value) { throw 'formats: the text field did not retain the value' }
}

function Assert-FmtPicker($dialog, [string]$phase) {
    $again = Get-FmtWindows $phase -Dialog
    if (-not $again -or -not [Windows.Automation.Automation]::Compare($again, $dialog)) {
        $h = if ($again) { [IntPtr]$again.Current.NativeWindowHandle } else { [IntPtr]$dialog.Current.NativeWindowHandle }
        Stop-FmtWindow 'the file dialog changed' $h
    }
}

function Get-FmtPickerField($dialog) {
    $ct = [Windows.Automation.ControlType]
    $fields = @(Find-FmtControls $dialog $ct::Edit @() -Id '1148')
    if (-not $fields.Count) {
        $hosts = @(Find-FmtControls $dialog $ct::ComboBox 'File name:' | Where-Object {
            $_.Current.AutomationId -cin '1148', 'FileNameControlHost'
        })
        if ($hosts.Count -gt 1) { Stop-FmtControls 'ambiguous filename field host' $hosts -Popup }
        if ($hosts.Count) { $fields = @(Find-FmtControls $hosts[0] $ct::Edit @()) }
    }
    if (-not $fields.Count) {
        # The native Import Playlist picker exposes even Edit and Button as Win32 Pane controls.
        $fields = @(Find-FmtControls $dialog $ct::Pane @() -Id '1148' | Where-Object {
            $_.Current.FrameworkId -ceq 'Win32' -and $_.Current.ClassName -ceq 'Edit'
        })
    }
    if ($fields.Count -ne 1 -or -not $fields[0].Current.IsEnabled) {
        Stop-FmtControls 'expected one enabled filename field' $fields -Popup
    }
    return $fields[0]
}

function Set-FmtPickerPath($dialog, [string]$phase, [string]$value) {
    Assert-FormatPath $value
    $field = Get-FmtPickerField $dialog
    Assert-FmtPicker $dialog $phase
    if ($field.Current.ControlType -eq [Windows.Automation.ControlType]::Edit) { Set-FmtValue $field $value; return }
    if (-not [RbspFormatInput]::SetPickerPath([IntPtr]$dialog.Current.NativeWindowHandle, [IntPtr]$field.Current.NativeWindowHandle, $value)) {
        throw 'formats: could not set and read back the verified native filename field'
    }
}

function Test-FmtPickerPath($dialog, [string]$phase, [string]$path) {
    $field = Get-FmtPickerField $dialog
    Assert-FmtPicker $dialog $phase
    if ($field.Current.ControlType -eq [Windows.Automation.ControlType]::Edit) {
        $value = $null
        return $field.TryGetCurrentPattern([Windows.Automation.ValuePattern]::Pattern, [ref]$value) -and $value.Current.Value -ceq $path
    }
    return [RbspFormatInput]::PickerPathMatches([IntPtr]$dialog.Current.NativeWindowHandle, [IntPtr]$field.Current.NativeWindowHandle, $path)
}

function Invoke-FmtPickerButton($dialog, [string]$phase, [string]$caption, [string]$id, [string]$path) {
    $ct = [Windows.Automation.ControlType]
    $buttons = @(Find-FmtControls $dialog $ct::Button $caption -Id $id)
    if (-not $buttons.Count) {
        $buttons = @(Find-FmtControls $dialog $ct::Pane $caption -Id $id | Where-Object {
            $_.Current.FrameworkId -ceq 'Win32' -and $_.Current.ClassName -ceq 'Button'
        })
    }
    if ($buttons.Count -ne 1 -or -not $buttons[0].Current.IsEnabled) { Stop-FmtControls 'expected one enabled picker button' $buttons -Popup }
    $button = $buttons[0]
    if ($path -and -not (Test-FmtPickerPath $dialog $phase $path)) {
        throw 'formats: the verified filename field does not match the kit path'
    }
    Assert-FmtPicker $dialog $phase
    $ok = if ($button.Current.ControlType -eq $ct::Button) { Invoke-El $button }
    else { [RbspFormatInput]::PressPickerButton([IntPtr]$dialog.Current.NativeWindowHandle, [IntPtr]$button.Current.NativeWindowHandle, [int]$id, $caption) }
    if (-not $ok) { throw 'formats: could not press the verified picker button' }
}

function Submit-FmtImportPicker([string]$value) {
    $step = $fmtProgress.Step
    $fmtProgress.Step = "$step-wait"
    $dialog = Wait-FmtWindow 'Open'
    $fmtProgress.Step = "$step-filename"
    Set-FmtPickerPath $dialog 'Open' $value
    $fmtProgress.Step = "$step-open"
    Invoke-FmtPickerButton $dialog 'Open' 'Open' '1' $value
    $fmtProgress.Step = $step
}

# Only during the operation that can produce each prompt, and only its exact text/button.
function Complete-FmtOperation([string]$phase) {
    if ($phase -cne 'Refusal') { throw 'formats: unexpected confirmation phase' }
    $ct = [Windows.Automation.ControlType]
    $dialog = Get-FmtWindows $phase -Refocus -Dialog
    if ($dialog) {
        $texts = @(Find-FmtControls $dialog $ct::Text @()) + @(Find-FmtControls $dialog $ct::Custom @())
        $captions = @($texts | ForEach-Object { $_.Current.Name -replace '\s+', ' ' })
        $buttons = @(Find-FmtControls $dialog $ct::Button @() | Where-Object {
            [Windows.Automation.TreeWalker]::ControlViewWalker.GetParent($_).Current.ControlType -ne $ct::TitleBar
        })
        $names = @($buttons.Current.Name)
        $affirm = if ($names.Count -eq 1 -and $names[0] -cin 'OK', 'Close') { $names[0] }
        $matchedText = @($captions -cmatch (Get-FormatRefusalPattern))
        if (-not $affirm -or $matchedText.Count -ne 1) {
            Stop-FmtControls 'unexpected import or playlist confirmation' ($buttons + $texts) -Popup
        }
        if ($phase -eq 'Refusal') { $fmtProgress.Refusal = $matchedText[0] }
        $button = Get-FmtControl $dialog $ct::Button $affirm -Popup
        $again = Get-FmtWindows $phase -Dialog
        if (-not $again -or -not [Windows.Automation.Automation]::Compare($again, $dialog) -or
            -not (Invoke-El $button)) {
            throw 'formats: could not acknowledge the verified rekordbox dialog'
        }
        Start-Sleep -Milliseconds 300
    }
    $null = Get-FmtWindows 'Main'
}

# rekordbox 7.2.20 exposes anonymous JUCE buttons at a regular pitch under
# ShowHideTreeShortcutButton. Match the entire column relative to that anchor.
function Get-FmtSourceLayout {
    $ct = [Windows.Automation.ControlType]
    $toggle = Get-FmtControl $fmtMain $ct::Button 'ShowHideTreeShortcutButton'
    $r = $toggle.Current.BoundingRectangle
    $window = $fmtMain.Current.BoundingRectangle
    $unit = $r.Height / 20.0
    if ($unit -le 0 -or -not $window.Contains($r) -or $r.Left - $window.Left -gt 12 * $unit -or
        $toggle.Current.FrameworkId -cne 'JUCE') { throw 'formats: the source shortcut anchor changed' }

    $containerId = ($toggle.Current.AutomationId -creplace '^ShowHideTreeShortcutButton', '') + ('.<empty>' * 3)
    $columns = @(Find-FmtControls $fmtMain $ct::Custom @() -Id $containerId | Where-Object {
        $b = $_.Current.BoundingRectangle
        $window.Contains($b) -and [math]::Abs($b.Left - ($r.Left - 4 * $unit)) -le $unit -and
        [math]::Abs($b.Top - ($r.Bottom + 2 * $unit)) -le $unit -and
        [math]::Abs($b.Width - 33 * $unit) -le $unit -and [math]::Abs($b.Height - 344 * $unit) -le $unit
    })
    if ($columns.Count -ne 1) { Stop-FmtControls 'expected one rekordbox source icon column' $columns }
    $column = $columns[0].Current.BoundingRectangle
    $buttons = @(Find-FmtControls $fmtMain $ct::Button @() -Id ($containerId + '.<empty>') | Where-Object {
        $column.Contains($_.Current.BoundingRectangle)
    } | Sort-Object { $_.Current.BoundingRectangle.Top })
    if ($buttons.Count -ne 17) { Stop-FmtControls 'expected 17 rekordbox source icons' $buttons }
    for ($i = 0; $i -lt $buttons.Count; $i++) {
        $c = $buttons[$i].Current
        $b = $c.BoundingRectangle
        if ($c.Name -cne '' -or ($i -lt 2 -and -not $c.IsEnabled) -or
            [math]::Abs($b.Left - ($column.Left + 5 * $unit)) -gt $unit -or
            [math]::Abs($b.Top - ($column.Top + (3 + 20 * $i) * $unit)) -gt $unit -or
            [math]::Abs($b.Width - 18 * $unit) -gt $unit -or [math]::Abs($b.Height - 18 * $unit) -gt $unit) {
            Stop-FmtControls 'rekordbox source icon positions changed' $buttons
        }
    }
    return @{ Buttons = $buttons; Left = $column.Right + 2 * $unit; Top = $column.Top; Unit = $unit }
}

function Find-FmtSource($layout, [string]$name) {
    $ct = [Windows.Automation.ControlType]
    $type = if ($name -ceq 'Collection') { $ct::Text } else { $ct::Group }
    $source = @(Find-FmtControls $fmtMain $type @() | Where-Object {
        $c = $_.Current
        $r = $_.Current.BoundingRectangle
        [math]::Abs($r.Left - $layout.Left) -le $layout.Unit -and
        [math]::Abs($r.Top - $layout.Top) -le $layout.Unit -and
        $(if ($name -ceq 'Collection') {
            (Get-FmtSourceName $c.Name) -ceq 'Collection' -and [math]::Abs($r.Height - 12 * $layout.Unit) -le $layout.Unit
        } else {
            $c.FrameworkId -ceq 'JUCE' -and $r.Width -gt 32 * $layout.Unit -and $r.Height -gt 71 * $layout.Unit -and
                $fmtMain.Current.BoundingRectangle.Contains($r)
        })
    })
    if (-not $source.Count) { return }
    if ($source.Count -ne 1 -or -not $source[0].Current.IsEnabled) {
        Stop-FmtControls "ambiguous or disabled $name source" $source
    }
    if ($name -ceq 'Playlists') {
        # This search button stays at the pane's upper left when JUCE provides
        # no usable name for the Playlists tree row. Control View makes it a sibling.
        $bounds = $source[0].Current.BoundingRectangle
        $search = @(Find-FmtControls $fmtMain $ct::Button 'SearchPlaylistButton' | Where-Object {
            $c = $_.Current
            $r = $c.BoundingRectangle
            $c.FrameworkId -ceq 'JUCE' -and $c.AutomationId -cmatch '^SearchPlaylistButton\.' -and $bounds.Contains($r) -and
                [math]::Abs($r.Left - $bounds.Left) -le $layout.Unit -and
                [math]::Abs($r.Top - $bounds.Top) -le $layout.Unit -and
                [math]::Abs($r.Width - 24 * $layout.Unit) -le $layout.Unit -and
                [math]::Abs($r.Height - 23 * $layout.Unit) -le $layout.Unit
        })
        if ($search.Count -gt 1 -or ($search.Count -eq 1 -and -not $search[0].Current.IsEnabled)) {
            Stop-FmtControls 'ambiguous or disabled Playlists search button' $search
        }
        if ($search.Count -eq 1) { return $source[0] }

        $rows = @(Find-FmtSourceRows $layout $source[0] 'Playlists' -ReadUnnamed)
        if (-not $rows.Count) { return }
        if ($rows.Count -ne 1 -or -not $rows[0].Current.IsEnabled) {
            Stop-FmtControls 'expected one enabled Playlists source row' $rows
        }
    }
    return $source[0]
}

function Find-FmtSourceRows($layout, $pane, [string]$name, [switch]$Hidden, [switch]$ReadUnnamed) {
    $ct = [Windows.Automation.ControlType]
    $source = $pane.Current.BoundingRectangle
    # The JUCE Control View flattens the tree rows into siblings of the pane.
    # The tab bar and playlist search field occupy the first 56 scaled pixels.
    $rows = @(foreach ($type in $ct::Custom, $ct::Text) {
        Find-FmtControls $fmtMain $type @() -Hidden:$Hidden | Where-Object {
            $c = $_.Current
            $r = $c.BoundingRectangle
            $c.FrameworkId -ceq 'JUCE' -and $source.Contains($r) -and $r.Top -ge ($source.Top + 55 * $layout.Unit) -and
                $r.Width -gt 0 -and $r.Height -gt 0 -and $r.Height -le 19 * $layout.Unit -and
                $r.Left -le ($source.Left + 32 * $layout.Unit)
        }
    })
    $named = @($rows | Where-Object { (Get-FmtSourceName $_.Current.Name) -ceq $name })
    if ($named.Count -or -not $ReadUnnamed) { return $named }
    foreach ($row in $rows) {
        $c = $row.Current
        $r = $c.BoundingRectangle
        $offset = ($r.Top - $source.Top) / $layout.Unit
        if ($c.Name -or $c.IsOffscreen -or $c.ControlType -ne $ct::Custom -or
            [math]::Abs($r.Left - $source.Left) -gt $layout.Unit -or
            [math]::Abs($r.Height / $layout.Unit - 15) -gt 1 -or $offset -lt 55 -or $offset -gt 102 -or
            [math]::Abs(($source.Right - $r.Right) / $layout.Unit - 12) -gt 1) { continue }
        if ((Get-FmtSourceName (Read-FmtClientText $r)) -ceq $name) { $row }
    }
}

function Get-FmtExplorePlaylist($layout, $pane) {
    if (-not $pane) { throw 'formats: the Playlists source changed' }
    $rows = @(Find-FmtSourceRows $layout $pane 'RBSP Tests' -ReadUnnamed)
    if ($rows.Count -ne 1 -or -not $rows[0].Current.IsEnabled) {
        Stop-FmtControls 'expected one enabled RBSP Tests playlist in the left source' $rows
    }
    return $rows[0]
}

function Confirm-FmtSource($layout, [string]$name, [int]$index) {
    $source = Find-FmtSource $layout $name
    if ($source) { return $source }
    Invoke-FmtClick $layout.Buttons[$index]
    $end = (Get-Date).AddSeconds(3)
    do {
        Start-Sleep -Milliseconds 100
        $null = Get-FmtWindows 'Main'
        $source = Find-FmtSource $layout $name
        if ($source) { return $source }
    } while ((Get-Date) -lt $end)
    Stop-FmtControls "could not verify the $name source after icon selection" @()
}

function Open-FmtPlaylistsSource($layout) {
    $pane = Find-FmtSource $layout 'Playlists'
    if ($pane) { return $pane }
    $null = Confirm-FmtSource $layout 'Collection' 0
    Confirm-FmtSource $layout 'Playlists' 1
}

function Read-FmtClientText($r, [int]$scale = 2) {
    . (Join-Path $Kit 'ocr-lib.ps1')
    $null = Get-FmtWindows 'Main'
    $client = [RbspWin]::UncoveredClient($fmtHandle)
    $left = [int][math]::Ceiling($r.Left)
    $top = [int][math]::Ceiling($r.Top)
    $width = [int][math]::Floor($r.Right) - $left
    $height = [int][math]::Floor($r.Bottom) - $top
    if ($left -lt $client.L -or $top -lt $client.T -or $left + $width -gt $client.R -or $top + $height -gt $client.B -or
        $width -le 0 -or $height -le 0) { throw 'formats: the playlist text is outside the verified rekordbox client area' }
    $bitmap = [System.Drawing.Bitmap]::new($width, $height)
    try {
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try { $graphics.CopyFromScreen($left, $top, 0, 0, $bitmap.Size) }
        finally { $graphics.Dispose() }
        $null = Get-FmtWindows 'Main'
        $null = [RbspWin]::UncoveredClient($fmtHandle)
        $text = (Read-BitmapText $bitmap 0 0 $width $height -Scale $scale).Trim()
        if ($text) { $text }
        if (-not $text) { (Read-BitmapText $bitmap 0 0 $width $height -Scale $scale -Plain).Trim() }
    } finally { $bitmap.Dispose() }
}

function Wait-FmtImportResult {
    $end = (Get-Date).AddMinutes(5)
    $idle = $null
    do {
        Start-Sleep -Milliseconds 200
        $state = Get-FmtWindowState 'ImportResult'
        if ($state.ImportProgress.Count -or ($state.Dialogs | Where-Object { $_.Current.Name -cin 'Open', 'Import Playlist' })) {
            $idle = $null
            continue
        }
        if (-not $idle) { $idle = Get-Date }
        if (((Get-Date) - $idle).TotalSeconds -ge 2) {
            Complete-FmtOperation 'Refusal'
            return
        }
    } while ((Get-Date) -lt $end)
    throw 'formats: rekordbox did not finish the import'
}

function Invoke-FmtImport {
    $lock = Open-FormatLock
    try {
        $dir = Join-Path (Join-Path $Kit 'corpus') 'formats'
        $staged = Get-FormatFiles $dir -Verify
        $manifest = Get-FormatManifest $dir
        $nonce = [guid]::NewGuid().ToString('N').Substring(0, 12)
        $state = [pscustomobject]@{
            manifest = $manifest
            name = 'RBSP Formats ' + $manifest.Substring(0, 16).ToLowerInvariant() + '-' + $nonce
            playlist = $null
        }
        $fmtProgress.Step = 'write-import-file'
        $inputDir = Join-Path (Split-Path -Parent $dir) ('format-input-' + $nonce)
        $inputFile = Join-Path $inputDir ($state.name + '.m3u8')
        Assert-FormatPath $inputFile
        [void][IO.Directory]::CreateDirectory($inputDir)
        Write-FormatM3U8 $inputFile $staged
        Write-FormatImportState $state
        $fmtProgress.Step = 'file-menu'
        Open-FmtExploreMenu 'File' -NoShortcut
        $fileMenu = @(Wait-FmtExploreMenu)
        $fmtProgress.Step = 'import-menu'
        $fileSurfaces = @(Get-FmtExploreMenuSurfaces $fileMenu)
        Open-FmtExploreMenu 'Import' $fileMenu
        $importMenu = @(Wait-FmtExploreMenu $fileSurfaces)
        $fmtProgress.Step = 'import-playlist-menu'
        Open-FmtExploreMenu 'Import Playlist' $importMenu
        $fmtProgress.Step = 'import-file-dialog'
        Submit-FmtImportPicker $inputFile
        $fmtProgress.Step = 'import-result'
        Wait-FmtImportResult
    } finally {
        try {
            if ($inputFile -and [IO.Directory]::Exists($inputDir)) {
                [IO.File]::Delete($inputFile)
                [IO.Directory]::Delete($inputDir)
            }
        } finally { $lock.Dispose() }
    }
}

function Invoke-RbFormatTask([string]$command, [string]$arg) {
    if ($command -cnotin 'fmtimport', 'fmtcancelpicker', 'fmtstep', 'fmtuidump') {
        throw 'formats: invalid desktop task'
    }
    Uia
    Add-Type -AssemblyName System.Windows.Forms, WindowsBase
    Screen-Dpi
    if (-not ('RbspFormatInput' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class RbspFormatInput {
    [StructLayout(LayoutKind.Sequential)]
    private struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)]
    private struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] private static extern IntPtr WindowFromPoint(Point p);
    [DllImport("user32.dll")] private static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll")] private static extern bool IsChild(IntPtr parent, IntPtr child);
    [DllImport("user32.dll")] private static extern IntPtr GetDlgItem(IntPtr dialog, int id);
    [DllImport("user32.dll", ExactSpelling = true)] private static extern bool PostMessageW(IntPtr h, uint message, UIntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr h, int command);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint thread, uint foregroundThread, bool attach);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr h, out Rect rect);
    [DllImport("user32.dll", ExactSpelling = true)] private static extern int GetWindowLongW(IntPtr h, int index);
    [DllImport("user32.dll", ExactSpelling = true)]
    private static extern IntPtr SendMessageTimeoutW(IntPtr h, uint message, UIntPtr wParam, IntPtr lParam,
                                                     uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, EntryPoint = "SendMessageTimeoutW")]
    private static extern IntPtr SendPickerText(IntPtr h, uint message, UIntPtr wParam, string value,
                                                uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, EntryPoint = "SendMessageTimeoutW")]
    private static extern IntPtr ReadPickerText(IntPtr h, uint message, UIntPtr wParam, StringBuilder value,
                                                uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)] private static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)] private static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] private static extern IntPtr GetMenu(IntPtr h);
    [DllImport("user32.dll")] private static extern IntPtr GetSubMenu(IntPtr menu, int position);
    [DllImport("user32.dll")] private static extern int GetMenuItemCount(IntPtr menu);
    [DllImport("user32.dll")] private static extern uint GetMenuState(IntPtr menu, uint position, uint flags);
    [DllImport("user32.dll")] private static extern bool GetMenuItemRect(IntPtr h, IntPtr menu, uint position, out Rect rect);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern int GetMenuStringW(IntPtr menu, uint position, StringBuilder label, int size, uint flags);
    [DllImport("user32.dll")] private static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] private static extern void mouse_event(uint flags, int x, int y, uint data, UIntPtr extra);
    public static string WindowClass(IntPtr h) {
        var s = new StringBuilder(256);
        GetClassNameW(h, s, s.Capacity);
        return s.ToString();
    }
    public static string WindowTitle(IntPtr h) {
        var s = new StringBuilder(256);
        GetWindowTextW(h, s, s.Capacity);
        return s.ToString();
    }
    public static string WindowSize(IntPtr h) {
        Rect rect;
        return GetWindowRect(h, out rect)
            ? (rect.Right - rect.Left) + "x" + (rect.Bottom - rect.Top)
            : "unknown";
    }
    private static bool PickerControl(IntPtr dialog, IntPtr control, string kind) {
        return dialog != IntPtr.Zero && control != IntPtr.Zero && GetForegroundWindow() == dialog &&
               WindowClass(dialog) == "#32770" && GetAncestor(control, 2) == dialog &&
               IsWindowVisible(control) && IsWindowEnabled(control) && WindowClass(control) == kind;
    }
    private static bool PickerField(IntPtr dialog, IntPtr edit) {
        IntPtr host = GetDlgItem(dialog, 1148);
        return PickerControl(dialog, edit, "Edit") && host != IntPtr.Zero && IsChild(host, edit);
    }
    public static bool PickerPathMatches(IntPtr dialog, IntPtr edit, string path) {
        if (!PickerField(dialog, edit)) return false;
        var buffer = new StringBuilder(path.Length + 2);
        IntPtr length;
        return ReadPickerText(edit, 0x000D, new UIntPtr((uint)buffer.Capacity), buffer, 3, 1000, out length) != IntPtr.Zero &&
               length.ToInt64() == path.Length && buffer.ToString() == path;
    }
    public static bool SetPickerPath(IntPtr dialog, IntPtr edit, string path) {
        IntPtr result;
        return PickerField(dialog, edit) && SendPickerText(edit, 0x000C, UIntPtr.Zero, path, 3, 1000, out result) != IntPtr.Zero &&
               result != IntPtr.Zero && PickerPathMatches(dialog, edit, path);
    }
    public static bool PressPickerButton(IntPtr dialog, IntPtr button, int id, string caption) {
        return PickerControl(dialog, button, "Button") && GetDlgItem(dialog, id) == button &&
               WindowTitle(button).Replace("&", "") == caption &&
               PostMessageW(button, 0x00F5, UIntPtr.Zero, IntPtr.Zero); // BM_CLICK
    }
    public static bool IsNonActivating(IntPtr h) {
        if ((GetWindowLongW(h, -20) & 0x08000000) != 0) return true; // GWL_EXSTYLE, WS_EX_NOACTIVATE
        // JUCE tooltips and shadows can suppress mouse input with HTTRANSPARENT.
        Rect rect;
        if (!GetWindowRect(h, out rect)) return false;
        int x = rect.Left + (rect.Right - rect.Left) / 2;
        int y = rect.Top + (rect.Bottom - rect.Top) / 2;
        var point = new IntPtr((x & 0xffff) | (y << 16));
        IntPtr hit;
        return SendMessageTimeoutW(h, 0x84, UIntPtr.Zero, point, 3, 50, out hit) != IntPtr.Zero &&
               hit.ToInt64() == -1; // WM_NCHITTEST, HTTRANSPARENT
    }
    public static bool ParkCursor(IntPtr h) {
        Rect rect;
        if (!GetWindowRect(h, out rect) || rect.Right <= rect.Left || rect.Bottom <= rect.Top) return false;
        return SetCursorPos(rect.Left + (rect.Right - rect.Left) / 2,
                            rect.Top + Math.Min(10, (rect.Bottom - rect.Top) / 2));
    }
    public static int FileMenuPoint(IntPtr h, out int x, out int y) {
        const uint byPosition = 0x400u;
        x = y = 0;
        IntPtr menu = GetMenu(h);
        if (menu == IntPtr.Zero) return 0;
        int count = GetMenuItemCount(menu);
        if (count < 0) return -1;
        int file = -1;
        var label = new StringBuilder(128);
        for (int i = 0; i < count; i++) {
            if (GetMenuStringW(menu, (uint)i, label, label.Capacity, byPosition) <= 0 ||
                label.ToString().Replace("&", "") != "File") continue;
            if (file >= 0) return -1;
            file = i;
        }
        if (file < 0) return 0;
        Rect rect;
        if (GetSubMenu(menu, file) == IntPtr.Zero ||
            (GetMenuState(menu, (uint)file, byPosition) & 3u) != 0 ||
            !GetMenuItemRect(h, menu, (uint)file, out rect) ||
            rect.Right <= rect.Left || rect.Bottom <= rect.Top) return -1;
        x = rect.Left + (rect.Right - rect.Left) / 2;
        y = rect.Top + (rect.Bottom - rect.Top) / 2;
        return 1;
    }
    public static bool PointAt(IntPtr expected, IntPtr target, int x, int y, out IntPtr foreground) {
        var p = new Point { X = x, Y = y };
        foreground = GetForegroundWindow();
        if (foreground != expected || GetAncestor(WindowFromPoint(p), 2) != target || !SetCursorPos(x, y)) return false;
        foreground = GetForegroundWindow();
        return foreground == expected && GetAncestor(WindowFromPoint(p), 2) == target;
    }
    public static bool Click(IntPtr expected, IntPtr target, int x, int y, bool right, out IntPtr foreground) {
        if (!PointAt(expected, target, x, y, out foreground)) return false;
        mouse_event(right ? 0x08u : 0x02u, 0, 0, 0, UIntPtr.Zero);
        mouse_event(right ? 0x10u : 0x04u, 0, 0, 0, UIntPtr.Zero);
        return true;
    }
}
'@
    }
    $fmtHandle, $fmtPid = Get-RbMain -FormatMenu:($command -eq 'fmtstep')
    $fmtMain = Get-FmtMainElement $fmtHandle $fmtPid
    $fmtStop = if ($command -ne 'fmtcancelpicker') { Join-Path $B 'midi-stop' }
    $fmtDeadline = (Get-Date).AddSeconds($(if ($command -eq 'fmtimport') { 420 } else { 45 }))
    $fmtExplore = @{ File = ''; Count = 0 }
    $fmtStep = @{ Mode = '' }
    $fmtProgress = @{ Step = 'focus' }
    try {
        if ($command -eq 'fmtcancelpicker') { return Cancel-FmtPickerForClose }
        if ($command -eq 'fmtstep') { return Invoke-FmtStep $arg }
        $null = Get-FmtWindows 'Main' -Refocus
        Move-FmtCursor
        $null = Get-FmtWindows 'Main'
        if ($command -eq 'fmtimport') {
            if ($arg) { throw 'formats: invalid import mode' }
            Invoke-FmtImport
            return $fmtProgress.Refusal
        }
        Invoke-FmtExplore
    } catch {
        $failure = $_
        $dump = try { Save-FmtStopDump -Step:($command -eq 'fmtstep') } catch { }
        try { Close-FmtMenu } catch { }
        $why = if ($failure.Exception.Message -clike 'formats: *') { $failure.Exception.Message } else {
            # name the real failure (type, script line, short message) so the next round needs no guessing
            $message = "formats: Windows automation failed: $($failure.Exception.GetType().Name) at line $($failure.InvocationInfo.ScriptLineNumber)"
            if ($command -notin 'fmtuidump', 'fmtstep') {
                if (-not $dump) { throw $failure }
                $msg = ($failure.Exception.Message -replace '[\r\n]+', ' ')
                if ($msg.Length -gt 160) { $msg = $msg.Substring(0, 160) + '...' }
                $message += " ($($failure.InvocationInfo.Line.Trim())): $msg"
            }
            $message
        }
        if ($fmtProgress.Refusal) { $why += "; import refusal: $($fmtProgress.Refusal)" }
        if ($dump) {
            $why += if ($command -eq 'fmtstep') { "; step capture: $dump" } else { "; UI dump ($($fmtProgress.Step)): $dump" }
        }
        if ($fmtExplore.File) { $why += "; explore capture: $($fmtExplore.Count) $($fmtExplore.File)" }
        throw $why
    }
}
