# watch-lib.ps1 (dot-source it after kit-lib.ps1): the dialog watch. The visible windows of the
# watched processes (and the test window), each "process | title | handle", against a baseline:
# a window not in it (another handle, even with the same title) is a STOP. During start, run,
# prepare and quit, the session task can close the verified update offer, rekordbox's verified
# news window (INFO) and rekordboxAgent's verified news window, and waits out rekordbox's verified STEMS engine progress window (up to 60 s,
# never touched). Each Watch- function returns
# its STOP line ('' when all is well); the caller Says a STOP and exits 1.
$UpdateProcess = 'Upmgr rekordbox'
$UpdateTitle = 'rekordbox update manager'
$UpdateWindow = '^' + [regex]::Escape("$UpdateProcess | $UpdateTitle | ") + '0x[0-9A-F]+$'
$InfoTitle = 'INFO'   # rekordbox's own news window (owner decision 2026-11-19)
$InfoWindow = '^' + [regex]::Escape("rekordbox | $InfoTitle | ") + '0x[0-9A-F]+$'
$AgentWindow = '^' + [regex]::Escape('rekordboxAgent | rekordboxAgent | ') + '0x[0-9A-F]+$'   # its news window (owner decision 2026-11-19)
$InitTitle = 'Initializing STEMS Engine'   # rekordbox's progress window, waited out (owner decision 2026-11-19)
$InitWindow = '^' + [regex]::Escape("rekordbox | $InitTitle | ") + '0x[0-9A-F]+$'
$InitWait = 60   # owner: rekordbox's normal progress bar, usually 5-10 s
$Watched = 'rekordbox', 'rekordboxAgent', $UpdateProcess, 'RB Stems Plus', 'WerFault', 'WerFaultSecure', 'smartscreen', 'consent'
$TestTitle = 'RBSP watch test'
$WatchBase = Join-Path $B 'watch-base.txt'   # run's baseline, for quit
$script:Base = [string[]]@()

function Assert-RbDialogReply([string]$reply) {
    if ($reply -cmatch '^\S+ (?:error|front): ((?:rekordbox #32770 message box|rekordbox Analysis Setting dialog): .+)$') { throw $Matches[1] }
}

# The watched windows now: a string[] (empty when none), $null when the list failed.
# A native rekordbox message box stops before any window-count or baseline check.
function WinList([switch]$AllowPicker) {
    $mode = if ($AllowPicker) { 'watch-quit' } else { 'watch' }
    $o = Session 'windows' 30 $mode
    Assert-RbDialogReply $o
    if ($o -notmatch 'windows: ') { return $null }
    $list = ($o -split 'windows: ', 2)[1]; if (-not $list) { return , ([string[]]@()) }
    $wins = @()
    foreach ($w in ($list -split ' \|\| ')) {
        # process | title | size | handle (a title may hold ' | ' itself)
        $f = @($w -split ' \| ')
        if ($f.Count -lt 4 -or $f[-1] -notmatch '^0x[0-9A-F]+$' -or $f[-2] -notmatch '^\d+x\d+$') { return $null }
        $t = $f[1..($f.Count - 3)] -join ' | '
        if ($Watched -contains $f[0] -or $t -eq $UpdateTitle -or $t -like "$TestTitle*") { $wins += $f[0] + ' | ' + $t + ' | ' + $f[-1] }
    }
    return , ([string[]]@($wins | Sort-Object -Unique))
}
function Assert-RbLoadWindows {
    if ($null -eq (WinList) -or -not (Test-RbRunning $sid)) { throw 'the window list failed after Load or rekordbox exited' }
}

function Watch-Line([string]$what, [string[]]$nw) {
    $l = "${what}: STOP: new window(s): " + ($nw -join '; ')
    if (-not @($nw | Where-Object { $_ -notlike "* | $TestTitle* | *" }).Count) { $l += ' (watch test)' }
    return $l + '; ' + (Session 'shot')
}
function Watch-RedactedLine([string]$what, [string[]]$windows, [string]$reason) {
    $handles = @($windows | ForEach-Object { ($_ -split ' \| ')[-1] } | Sort-Object)
    return "${what}: STOP: $($windows.Count) window(s); handles: $($handles -join ', '); $reason; " + (Session 'shot')
}
function Watch-Failed([string]$what) { return "${what}: STOP: the window list failed (the account signed out?)" }
# One verified window closed by the session task (CMD HANDLE): a RELATED window that isn't EXACT,
# or more than one, is a STOP; so is a failed close, or the same handle still listed after it.
function Watch-Dismiss([string]$what, [ref]$windows, [string[]]$related, [string]$exact, [string]$cmd) {
    $other = @($related -cnotmatch $exact)
    if ($other.Count) { return (Watch-Line $what $other) }
    if (-not $related.Count) { return '' }
    if ($related.Count -gt 1) { return (Watch-Line $what $related) }
    $handle = ($related[0] -split ' \| ')[-1]
    $r = Session $cmd 90 $handle
    if ($r -notmatch ('^\S+ ' + $cmd + ': closed; ')) {
        return "${what}: STOP: $r; " + (Session 'shot')
    }
    [void](Say "${what}: $r")
    $after = WinList
    if ($null -eq $after) { return (Watch-Failed $what) }
    $sameHandle = @($after -like "* | $handle")
    if ($sameHandle.Count) { return (Watch-Line $what $sameHandle) }
    # Only the closed window is cleared from the original list. Report other windows from either poll.
    $windows.Value = [string[]]@(($windows.Value -ne $related[0]) + $after | Sort-Object -Unique)
    return ''
}
# the verified update offer, then rekordbox's verified news window (INFO), closed
# rekordbox's verified "Initializing STEMS Engine" progress window(s) waited out: each verified
# (read only, stemsinit), then the list polled every 2 s until none is left, for up to $InitWait s.
# Nothing is clicked or closed. A look-alike, a failed check or a window still there is a STOP.
function Watch-Init([string]$what, [ref]$windows) {
    $now = $windows.Value; $seen = @(); $end = $null
    while ($true) {
        $init = @($now | Where-Object { $_ -like "* | $InitTitle | 0x*" })
        if (-not $init.Count) { break }
        $other = @($init -cnotmatch $InitWindow)
        if ($other.Count) { return (Watch-Line $what $other) }
        foreach ($w in @($init | Where-Object { $seen -notcontains $_ })) {
            $r = Session 'stemsinit' 30 (($w -split ' \| ')[-1])
            if ($r -match '^\S+ stemsinit: gone; ') { $seen += $w; continue }   # closed before its check
            if ($r -notmatch '^\S+ stemsinit: ok; ') { return "${what}: STOP: $r; " + (Session 'shot') }
            [void](Say "${what}: waiting for rekordbox's STEMS engine: $r")
            $seen += $w
        }
        if ($null -eq $end) { $end = (Get-Date).AddSeconds($InitWait) }
        elseif ((Get-Date) -ge $end) { return "${what}: STOP: rekordbox still initializing its STEMS engine after $InitWait s: " + ($init -join '; ') + '; ' + (Session 'shot') }
        Start-Sleep -Seconds 2
        $now = WinList; if ($null -eq $now) { return (Watch-Failed $what) }
    }
    if (-not $seen.Count) { return '' }
    [void](Say "${what}: rekordbox's STEMS engine progress window(s) gone")
    # the progress windows cleared; windows from either list stay for the usual checks
    $windows.Value = [string[]]@(@($windows.Value | Where-Object { $seen -notcontains $_ }) + @($now) | Sort-Object -Unique)
    return ''
}
function Watch-Update([string]$what, [ref]$windows) {
    $s = Watch-Init $what $windows; if ($s) { return $s }
    $up = @($windows.Value | Where-Object { $_ -like "$UpdateProcess | *" -or $_ -like "* | $UpdateTitle | 0x*" })
    if ($up.Count -and $env:RBKIT_STOP_ON_UPDATE -eq '1' -and $what -ne 'quit') { return (Watch-Line $what $up) }
    $s = Watch-Dismiss $what $windows $up $UpdateWindow 'closeupdate'; if ($s) { return $s }
    $info = @($windows.Value | Where-Object { $_ -like "* | $InfoTitle | 0x*" })
    $s = Watch-Dismiss $what $windows $info $InfoWindow 'closeinfo'; if ($s) { return $s }
    $agent = @($windows.Value | Where-Object { $_ -like 'rekordboxAgent | *' })
    return (Watch-Dismiss $what $windows $agent $AgentWindow 'closeagentnews')
}
# the baseline taken. -Allow: a window already open would never look new, so only rekordbox's main
# window ("rekordbox | rekordbox": exactly one with -Rekordbox, else at most one) and our app's
# main window (at most one) may be there. -News permits the one agent window that start closes.
# Before MIDI, only check for updater windows. After a Close, apply the usual window checks too.
function Watch-Start([string]$what, [switch]$Allow, [switch]$Rekordbox, [switch]$News, [switch]$BeforeMidi, [switch]$RedactTitles) {
    $l = WinList; if ($null -eq $l) { return (Watch-Failed $what) }
    if ($what -in 'start', 'run', 'prepare', 'quit') {
        $hadOffer = $BeforeMidi -and (@($l -cmatch $UpdateWindow).Count + @($l -cmatch $InfoWindow).Count)
        $s = Watch-Update $what ([ref]$l); if ($s) { return $s }
        if ($BeforeMidi -and -not $hadOffer) { return '' }
    }
    $script:Base = $l
    if ($Allow) {
        $rb = @($l | Where-Object { $_ -match '^rekordbox \| rekordbox \| 0x[0-9A-F]+$' })
        $app = @($l | Where-Object { $_ -match '^RB Stems Plus \| RB Stems Plus \| 0x[0-9A-F]+$' })
        $agent = @($l -match '^rekordboxAgent \| rekordboxAgent \| 0x[0-9A-F]+$')
        $newsCount = if ($News -and $agent.Count -eq 1) { 1 } else { 0 }
        if ($rb.Count + $app.Count + $newsCount -ne $l.Count -or $rb.Count -gt 1 -or ($Rekordbox -and $rb.Count -ne 1) -or $app.Count -gt 1) {
            if ($RedactTitles) { return (Watch-RedactedLine $what $l 'unexpected windows at the start') }
            return "${what}: STOP: not just rekordbox's$(if (-not $Rekordbox) { ' (if open)' }) and our app's main windows at the start: " + ($l -join '; ') + '; ' + (Session 'shot')
        }
    }
    return ''
}
function Watch-Stop([string]$what, [switch]$RedactTitles) {
    $now = WinList; if ($null -eq $now) { return (Watch-Failed $what) }
    if ($what -in 'run', 'prepare', 'quit') {
        $s = Watch-Update $what ([ref]$now); if ($s) { return $s }
    }
    $nw = [string[]]@($now | Where-Object { $script:Base -notcontains $_ })
    if ($nw.Count) {
        if ($RedactTitles) { return (Watch-RedactedLine $what $nw 'new windows') }
        return (Watch-Line $what $nw)
    }
    return ''
}
function Watch-Save { Set-Content -LiteralPath $WatchBase -Value ($script:Base -join "`n") }
# quit's check: against run's baseline when there is one, else the -Allow rule
function Watch-Quit([switch]$RedactTitles) {
    if (Test-Path -LiteralPath $WatchBase) {
        $script:Base = [string[]]@(Get-Content -LiteralPath $WatchBase | Where-Object { $_ })
        return (Watch-Stop 'quit' -RedactTitles:$RedactTitles)
    }
    return (Watch-Start 'quit' -Allow -Rekordbox -RedactTitles:$RedactTitles)
}
