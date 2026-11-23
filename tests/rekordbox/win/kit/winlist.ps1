# Read-only apart from the kit's own screenshots and session-task reply. No input.
function Save-RbWindowCrop([IntPtr]$h, [int]$processId, [int[]]$allowedPids, [RbspWin+RECT]$rect, $screen, [string]$file) {
    $bounds = [Drawing.Rectangle]::FromLTRB($rect.L, $rect.T, $rect.R, $rect.B)
    $crop = [Drawing.Rectangle]::Intersect($screen, $bounds)
    if ($crop.Width -le 0 -or $crop.Height -le 0) { return 'no-on-screen-area' }
    if (-not [RbspWin]::OrdinaryDesktop()) { return 'ordinary-desktop-unavailable' }
    if (-not [RbspWin]::NoForeignOverlap($h, $allowedPids, $crop.Left, $crop.Top, $crop.Right, $crop.Bottom)) {
        return 'another-app-overlaps'
    }
    Assert-FormatPath $file
    $bitmap = [Drawing.Bitmap]::new($crop.Width, $crop.Height)
    try {
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try { $graphics.CopyFromScreen($crop.Left, $crop.Top, 0, 0, $bitmap.Size) }
        finally { $graphics.Dispose() }
        $after = New-Object RbspWin+RECT
        if ([RbspWin]::Pid($h) -ne $processId -or -not [RbspWin]::IsWindowVisible($h) -or
            -not [RbspWin]::GetWindowRect($h, [ref]$after) -or -not $rect.Equals($after) -or
            [RbspWin]::IsIconic($h) -or [RbspWin]::IsCloaked($h) -ne $false -or -not [RbspWin]::OrdinaryDesktop() -or
            -not [RbspWin]::NoForeignOverlap($h, $allowedPids, $crop.Left, $crop.Top, $crop.Right, $crop.Bottom)) {
            return 'window-or-desktop-changed'
        }
        $bitmap.Save($file, [Drawing.Imaging.ImageFormat]::Png)
        return 'saved'
    } finally { $bitmap.Dispose() }
}

function Protect-RbDialogText([string]$text) {
    $media = '(?:mp3|m4a|aac|mp4|flac|wav|aiff?|ogg|opus|wma|alac|m3u8?|aifc|dsf|dff)'
    $pathStart = '(?<![\w\\/])(?:[A-Z]:[\\/]|\\\\|\.\.?[\\/]|[/\\](?=[\w.~])|[^\s\\/:]+[\\/](?=\S))'
    $pathChars = '[^\r\n<>|"''\u2018\u2019\u201c\u201d]'
    $text = $text -replace ('["\u201c][^"\u201d\r\n]+\.' + $media + '["\u201d]|[''\u2018][^''\u2019\r\n]+\.' + $media + '[''\u2019]'), '<redacted path>'
    $text = $text -replace '"[^"\r\n]*"|\u201c[^\u201d\r\n]*\u201d|(?<!\w)''[^''\r\n]*''(?!\w)|\u2018[^\u2019\r\n]*\u2019', '<redacted title>'
    $text = $text -replace ($pathStart + $pathChars + '*?\.[a-z0-9]{1,8}(?=$|[\s,.;)"''\u2019\u201d])'), '<redacted path>'
    $text = $text -replace ($pathStart + $pathChars + '+'), '<redacted path>'
    $text = $text -replace ('(?m)(?<prefix>^[ \t]*|:[ \t]*|[.!?][ \t]+)(?:(?!:|[.!?][ \t])[^\r\n])*?\.' + $media + '\b'), '${prefix}<redacted path>'
    $text = $text -replace '(?m)(?<label>\b(?:track|song)(?:\s+title)?[ \t]*:[ \t]*)(?![ \t]*<redacted)[^\r\n]+', '${label}<redacted title>'
    return $text
}

function Get-RbDialogControls([IntPtr]$dialog, [int]$processId, [switch]$RawText) {
    foreach ($child in [RbspWin]::DialogChildren($dialog, $processId)) {
        if ([RbspWin]::Pid($child) -ne $processId) { continue }
        $class = [RbspWin]::WindowClass($child)
        $id = [RbspWin]::GetDlgCtrlID($child)
        $style = [RbspWin]::GetWindowLongW($child, -16)
        $text = $null
        $hasText = $class -ceq 'Button' -or ($class -ceq 'Static' -and ($style -band 0x1F) -notin 3, 14, 15)
        if ($hasText) {
            $text = [RbspWin]::DialogControlText($dialog, $child, $processId, $id)
            if (-not $RawText -and $null -ne $text) { $text = Protect-RbDialogText $text }
        }
        [ordered]@{
            Handle = '0x' + $child.ToString('X')
            Class = $class
            Id = $id
            Style = $style
            Text = $text
            TextRead = $hasText -and $null -ne $text
            Visible = [RbspWin]::IsWindowVisible($child)
            Enabled = [RbspWin]::IsWindowEnabled($child)
        }
    }
}

function Get-RbDirectUiContent([IntPtr]$dialog, [int]$processId, [switch]$RawText, [switch]$AllElements, [switch]$ExcludeTitleBar) {
    try {
        Uia
        $A = [Windows.Automation.AutomationElement]
        $CT = [Windows.Automation.ControlType]
        $root = $A::FromHandle($dialog)
        if ($root.Current.ProcessId -ne $processId -or [int64]$root.Current.NativeWindowHandle -ne $dialog.ToInt64()) {
            return @{ Status = 'dialog-changed' }
        }
        $condition = if ($AllElements) { [Windows.Automation.Condition]::TrueCondition } else {
            [Windows.Automation.OrCondition]::new([Windows.Automation.Condition[]]@(
                [Windows.Automation.PropertyCondition]::new($A::ControlTypeProperty, $CT::Text)
                [Windows.Automation.PropertyCondition]::new($A::ControlTypeProperty, $CT::Button)
                [Windows.Automation.PropertyCondition]::new($A::ControlTypeProperty, $CT::Hyperlink)
            ))
        }
        $elements = @(foreach ($element in $root.FindAll([Windows.Automation.TreeScope]::Descendants, $condition)) {
            $control = $element.Current
            if ($control.ProcessId -ne $processId -or $control.IsOffscreen -or (-not $AllElements -and -not $control.Name)) { continue }
            if ($ExcludeTitleBar -and ($control.ControlType -eq $CT::TitleBar -or
                [Windows.Automation.TreeWalker]::ControlViewWalker.GetParent($element).Current.ControlType -eq $CT::TitleBar)) { continue }
            $entry = [ordered]@{
                Type = $control.ControlType.ProgrammaticName
                Text = if ($RawText) { $control.Name } else { Protect-RbDialogText $control.Name }
                Enabled = $control.IsEnabled
            }
            if ($AllElements -and $control.ControlType -eq $CT::Image) {
                $entry.Focusable = $control.IsKeyboardFocusable
                $entry.Patterns = @($element.GetSupportedPatterns() | ForEach-Object { $_.ProgrammaticName })
                if ($entry.Patterns.Count -eq 1 -and $entry.Patterns[0] -ceq 'ValuePatternIdentifiers.Pattern') {
                    $entry.ReadOnlyValue = $element.GetCurrentPattern([Windows.Automation.ValuePattern]::Pattern).Current.IsReadOnly
                }
            }
            $entry
        })
        if ([RbspWin]::Pid($dialog) -ne $processId -or [RbspWin]::WindowClass($dialog) -cne '#32770') {
            return @{ Status = 'dialog-changed' }
        }
        return [ordered]@{ Status = 'read'; Elements = $elements }
    } catch {
        return @{ Status = 'unavailable' }
    }
}

function Get-RbDialogFacts([IntPtr]$h, [int]$processId, [bool]$visible, [switch]$RawText) {
    $facts = [ordered]@{ Controls = @(Get-RbDialogControls $h $processId -RawText:$RawText) }
    if ($visible -and @($facts.Controls | Where-Object { $_.Class -ceq 'DirectUIHWND' -and $_.Visible }).Count) {
        $facts.DirectUI = Get-RbDirectUiContent $h $processId -RawText:$RawText
    }
    return $facts
}

function Get-RbErrorUiaSummary($content) {
    $known = @('rekordbox', 'Unexpected application error', 'OK', 'Close')
    [ordered]@{
        Status = $content.Status
        Elements = @(foreach ($element in $content.Elements) {
            $name = [Array]::IndexOf($known, $element.Text)
            $entry = [ordered]@{
                Type = $element.Type
                Name = if ($name -ge 0) { $known[$name] } elseif ($element.Text) { '<redacted>' } else { '' }
                Enabled = $element.Enabled
            }
            if ($element.Type -ceq 'ControlType.Image') {
                $entry.Focusable = $element.Focusable
                $entry.Patterns = $element.Patterns
                $entry.ReadOnlyValue = $element.ReadOnlyValue
            }
            $entry
        })
    }
}

# The measurement watch emits only exact known messages and hashes of other text.
function Get-RbMessageBoxes([int]$sessionId, [switch]$AllowPicker) {
    $KnownRbMessages = @('Unexpected application error')
    Initialize-RbspWin
    foreach ($process in (Get-RbProcess $sessionId)) {
        $fmtPid = $process.Id   # Test-FmtPickerOwner uses this process for the entire owner chain.
        foreach ($h in [RbspWin]::HandlesForProcess($fmtPid)) {
            if ([RbspWin]::Pid($h) -ne $fmtPid -or [RbspWin]::WindowClass($h) -cne '#32770' -or
                -not [RbspWin]::IsWindowVisible($h) -or [RbspWin]::IsCloaked($h) -eq $true) { continue }
            $text = @()
            try {
                $facts = Get-RbDialogFacts $h $fmtPid $true -RawText
                if ($AllowPicker -and (Get-FmtPickerTitles 'ClosePicker') -ccontains [RbspWin]::WindowCaption($h) -and
                    (Test-FmtPickerOwner $h) -and
                    @($facts.Controls | Where-Object { $_.Visible -and $_.Id -eq 1148 -and $_.Class -cin 'Edit', 'ComboBox' }).Count -and
                    @($facts.Controls | Where-Object { $_.Visible -and $_.Enabled -and $_.Id -eq 2 -and
                        $_.Class -ceq 'Button' -and $_.Text -cin 'Cancel', '&Cancel' }).Count) { continue }
                if ([RbspWin]::Pid($h) -eq $fmtPid -and [RbspWin]::WindowClass($h) -ceq '#32770') {
                    $text += @($facts.Controls | Where-Object { $_.Visible -and $_.TextRead -and $_.Class -ceq 'Static' } |
                        ForEach-Object { $_.Text })
                    $text += @($facts.DirectUI.Elements | Where-Object Type -ceq 'ControlType.Text' | ForEach-Object { $_.Text })
                }
            } catch { $text = @() }
            $detail = 'dialog text unavailable'
            $messages = @($text | Where-Object { $_ })
            if ($messages.Count) {
                $sha = [Security.Cryptography.SHA256]::Create()
                try {
                    $summaries = foreach ($message in $messages) {
                        $known = [Array]::IndexOf($KnownRbMessages, $message)
                        if ($known -ge 0) { $KnownRbMessages[$known] }
                        else {
                            $bytes = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($message))
                            $prefix = ([BitConverter]::ToString($bytes, 0, 6) -replace '-').ToLowerInvariant()
                            "unrecognised text ($($message.Length) chars, sha256:$prefix)"
                        }
                    }
                    $detail = ($summaries | Select-Object -Unique) -join ' / '
                } finally { $sha.Dispose() }
            }
            "0x$($h.ToString('X')): $detail"
        }
    }
}

function Get-RbWindowList([int]$sessionId) {
    Screen-Dpi
    Initialize-RbspWin
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    $screen = [Windows.Forms.SystemInformation]::VirtualScreen
    $foreground = [RbspWin]::GetForegroundWindow()
    $shotDir = Join-Path $B 'shots'
    $tag = [guid]::NewGuid().ToString('N')
    $processIds = @(Get-RbProcess $sessionId | ForEach-Object { $_.Id })
    $windows = @(foreach ($processId in $processIds) {
        foreach ($h in [RbspWin]::HandlesForProcess($processId)) {
            if ([RbspWin]::Pid($h) -ne $processId) { continue }
            $handle = '0x' + $h.ToString('X')
            $rect = New-Object RbspWin+RECT
            $hasRect = [RbspWin]::GetWindowRect($h, [ref]$rect)
            $coords = if ($hasRect) { @($rect.L, $rect.T, $rect.R, $rect.B) }
            $row = [ordered]@{
                Pid = $processId
                Handle = $handle
                Class = [RbspWin]::WindowClass($h)
                Owner = '0x' + [RbspWin]::GetWindow($h, 4).ToString('X') # GW_OWNER
                Visible = [RbspWin]::IsWindowVisible($h)
                Enabled = [RbspWin]::IsWindowEnabled($h)
                Foreground = $h -eq $foreground
                Minimized = [RbspWin]::IsIconic($h)
                Rect = $coords
                Style = '0x{0:X8}' -f [RbspWin]::GetWindowLongW($h, -16) # GWL_STYLE
                ExStyle = '0x{0:X8}' -f [RbspWin]::GetWindowLongW($h, -20) # GWL_EXSTYLE
                Cloaked = [RbspWin]::IsCloaked($h)
                Caption = Get-FmtSafeCaption ([RbspWin]::WindowCaption($h))
                Capture = 'unavailable'
                Shot = $null
            }
            if ($row.Class -ceq '#32770') {
                $facts = Get-RbDialogFacts $h $processId $row.Visible
                $row.Controls = $facts.Controls
                if ($facts.DirectUI) { $row.DirectUI = $facts.DirectUI }
                if ($row.Visible -and $row.Caption -ceq 'rekordbox' -and $row.Owner -ceq '0x0' -and $facts.DirectUI) {
                    $row.ErrorUia = Get-RbErrorUiaSummary (Get-RbDirectUiContent $h $processId -RawText -AllElements -ExcludeTitleBar)
                }
            }
            if ($row.Visible -and -not $row.Minimized -and $row.Cloaked -eq $false -and $hasRect) {
                $name = "winlist-$tag-$handle.png"
                try { $row.Capture = Save-RbWindowCrop $h $processId $processIds $rect $screen (Join-Path $shotDir $name) }
                catch { $row.Capture = 'capture-failed' }
                if ($row.Capture -ceq 'saved') { $row.Shot = $name }
            }
            if ([RbspWin]::Pid($h) -eq $processId) { $row }
        }
    })
    [ordered]@{ Processes = $processIds; Windows = $windows } | ConvertTo-Json -Depth 6 -Compress
}
