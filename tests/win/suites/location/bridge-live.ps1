# As the user, stick A plugged in and config.ini naming its folder: the bounded release measure
# through the DLL's nl_rekordbox_running and nl_process_start (review N3R5), with a stand-in
# rekordbox.exe (a copy of PING.EXE so named, never rekordbox itself). No rekordbox: not running,
# bridge-state never read. The stand-in running, a bridge-state for the chosen folder at its pid and
# creation time (nl_process_start's, checked against Windows' own): live, its state, reason and the
# drive's label. The same pid with start=-1: none live. A second stand-in that quit with its handle
# still open: its start time is -1. After closing the handle, bridge-state at that pid with start=-1
# (review NL3): none live, with rekordbox still running. The stand-in gone: not running again.
$null = Read-Sticks
$dir = Join-Path $env:TEMP 'rbsp-n3\rekordbox'
New-Item -ItemType Directory -Force $dir | Out-Null
$rb = Join-Path $dir 'rekordbox.exe'
Copy-Item -LiteralPath "$env:WINDIR\System32\PING.EXE" -Destination $rb -Force
# a process's creation time in seconds since 1970, as Windows' own process object gives it
function Get-UnixStart([int]$id) { return [DateTimeOffset]::new((Get-Process -Id $id).StartTime).ToUnixTimeSeconds() }
function Start-StandIn {
    $p = Start-Process -FilePath $rb -ArgumentList '-n', '300', '127.0.0.1' -WindowStyle Hidden -PassThru
    Check (-not $p.HasExited) "the stand-in rekordbox.exe runs (pid $($p.Id))" | Write-Host
    return $p
}
function Set-BridgeState([string]$sup, [string]$text) {
    $logs = Join-Path $sup 'Logs'
    New-Item -ItemType Directory -Force $logs | Out-Null
    [IO.File]::WriteAllText((Join-Path $logs 'bridge-state'), $text)
}
Check (@(Get-Process -Name rekordbox -ErrorAction SilentlyContinue).Count -eq 0) 'no rekordbox.exe runs before the stand-in'
foreach ($a in Get-LocArchs) {
    $sup = Get-LocSupport $a
    $cache = Get-LocCache $a
    $f = Get-Content -Raw -LiteralPath (Join-Path $sup '..\facts.json') | ConvertFrom-Json
    $avail = "^0 1 0 -?\d+\|RBSTICK\|$(Rx $f.gdir)$"
    # no rekordbox: a bridge-state there is never read
    Set-BridgeState $sup "pid=4`nstart=0`nstate=available`nlocation=$($f.gdir)`n"
    Expect (Loc $a 'measure-live' 'measure' @($sup, $cache)) $avail "the measure, no rekordbox ($a)"
    Expect (Last-Line 'bridge') '^0 0\|\|\|-$' "no rekordbox: not running, no bridge state ($a)"
    $p = Start-StandIn
    try {
        $s = Get-UnixStart $p.Id
        Expect (Loc $a 'start' 'start' @("$($p.Id)")) "^$s$" "nl_process_start of the stand-in: its creation time ($a)"
        # a live bridge-state: this pid and its creation time, the chosen folder
        Set-BridgeState $sup "pid=$($p.Id)`nstart=$s`nstate=unavailable`nlocation=$($f.gdir)`nreason=read-only`n"
        Expect (Loc $a 'measure-live' 'measure' @($sup, $cache)) $avail "the measure, the stand-in running ($a)"
        Expect (Last-Line 'bridge') '^1 1\|unavailable\|read-only\|RBSTICK$' "a live bridge-state: running, its state, reason and drive ($a)"
        # the same pid with start=-1: not its creation time
        Set-BridgeState $sup "pid=$($p.Id)`nstart=-1`nstate=available`nlocation=$($f.gdir)`n"
        Expect (Loc $a 'measure-live' 'measure' @($sup, $cache)) $avail "the measure, start=-1 at the live pid ($a)"
        Expect (Last-Line 'bridge') '^1 0\|\|\|-$' "start=-1 at the stand-in's pid: none live ($a)"
        $q = Start-StandIn
        $qid = $q.Id
        try {
            # Acquire the handle before exit and keep it until the start check returns.
            $null = $q.Handle
            Stop-Process -Id $qid -Force
            Check ($q.WaitForExit(30000)) "the second stand-in quit (pid $qid)"
            Expect (Loc $a 'start' 'start' @("$qid")) '^-1$' "nl_process_start of the exited stand-in with an open handle: -1 ($a)"
        } finally {
            $q.Dispose()
        }
        # Once the last handle is closed, keep the no-such-process checks (NL3).
        $q = $null; [GC]::Collect(); [GC]::WaitForPendingFinalizers()
        Check (Wait-Until { -not (Get-Process -Id $qid -ErrorAction SilentlyContinue) } 30) "no process at pid $qid"
        Expect (Loc $a 'start' 'start' @("$qid")) '^-1$' "nl_process_start of a pid with no process: -1 ($a)"
        Set-BridgeState $sup "pid=$qid`nstart=-1`nstate=available`nlocation=$($f.gdir)`n"
        Expect (Loc $a 'measure-live' 'measure' @($sup, $cache)) $avail "the measure, start=-1 at a pid with no process ($a)"
        Expect (Last-Line 'bridge') '^1 0\|\|\|-$' "NL3: start=-1 and no such process: none live, rekordbox running ($a)"
    } finally {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }
    Check (Wait-Until { -not (Get-Process -Id $p.Id -ErrorAction SilentlyContinue) } 30) "the stand-in quit (pid $($p.Id))"
    Expect (Loc $a 'measure-live' 'measure' @($sup, $cache)) $avail "the measure, the stand-in gone ($a)"
    Expect (Last-Line 'bridge') '^0 0\|\|\|-$' "the stand-in gone: not running, no bridge state ($a)"
    Remove-Item -LiteralPath (Join-Path $sup 'Logs\bridge-state') -Force
}
Done
