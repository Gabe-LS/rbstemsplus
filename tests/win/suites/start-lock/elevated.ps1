# N12 item 2 (the GPT pre-release audit; app.md section 5.9, I35): a start with the full token, as
# "Run as administrator" gives (user-high), is refused before anything is written. The stub started
# so starts the app elevated; the app's token check (core_win_token_check, Startup.cs) refuses:
# its TaskDialog (window title RB Stems Plus, main instruction T_CANT_OPEN, the Windows column of
# T_START_ROOT), and once its OK is pressed it ends with exit code 1. Nothing written meanwhile:
# the support folder (every file's size and time, app.log included) as it was, no one-copy window
# left, no app process left (the watcher's own log aside); app.log's length and bytes as before.
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
$me = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
Check ($me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) 'this step runs elevated (the full token, as Run as administrator gives)'
Stop-App
function Get-SupportTree {
    if (-not (Test-Path -LiteralPath $l.Support)) { return '-' }
    # (the watcher, running since the install, writes its own log whenever a round runs)
    # files with their size and time, folders as "path\ dir" (a folder made is a write too)
    return (@(Get-ChildItem -LiteralPath $l.Support -Recurse -Force | Where-Object { $_.Name -ne 'watcher.log' } | Sort-Object FullName | ForEach-Object {
                if ($_.PSIsContainer) { "$($_.FullName.Substring($l.Support.Length))\ dir" }
                else { "$($_.FullName.Substring($l.Support.Length)) $($_.Length) $($_.LastWriteTimeUtc.Ticks)" } }) -join "`n")
}
# app.log's bytes as they are (their SHA-256), so a rewrite or a cut shows, not only an addition
$alog = Join-Path $l.Support 'Logs\app.log'
function Get-LogHead([long]$n) {
    if (-not (Test-Path -LiteralPath $alog)) { return '-' }
    $f = New-Object IO.FileStream($alog, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]'ReadWrite, Delete'))
    try {
        $b = New-Object byte[] ([Math]::Min($n, $f.Length))
        $got = $f.Read($b, 0, $b.Length)
        $h = [Security.Cryptography.SHA256]::Create().ComputeHash($b, 0, $got)
        return "$got " + [BitConverter]::ToString($h).Replace('-', '')
    } finally { $f.Dispose() }
}
$before = Get-SupportTree
$logBefore = Get-AppLogSize
$headBefore = Get-LogHead $logBefore
$ver = (Get-Content -Raw -LiteralPath $l.Current).Trim()
$exe = Join-Path (Join-Path $l.Apps $ver) 'RB Stems Plus.exe'
[void](Start-Process -FilePath $l.Stub -PassThru)
$dlg = $null
$seen = Wait-Until {
    $script:dlg = @(Get-Process -Name 'RB Stems Plus' -ErrorAction SilentlyContinue | Where-Object {
            $p = $null; try { $p = $_.Path } catch { }
            $p -and $p.Equals($exe, [StringComparison]::OrdinalIgnoreCase) -and $_.MainWindowHandle -ne [IntPtr]::Zero }) | Select-Object -First 1
    $null -ne $script:dlg
} 60
Check $seen "the elevated start (the app, $exe) shows a window"
if (-not $dlg) { Done }
$null = $dlg.Handle   # kept, for its exit code
"its window title: '$($dlg.MainWindowTitle)'"
Check ($dlg.MainWindowTitle -eq 'RB Stems Plus') 'the window is the refusal TaskDialog (titled RB Stems Plus), not the main window'
$cant = Get-CoreText 'T_CANT_OPEN'
$root = Get-CoreText 'T_START_ROOT'
Check (Find-UiText $dlg.Id ('^' + [regex]::Escape($cant) + '$') 20) "its main instruction: $cant"
Check (Find-UiText $dlg.Id ('^' + [regex]::Escape($root) + '$') 10) "its text: $root"
Check (-not (Find-UiText $dlg.Id ('^' + [regex]::Escape((Get-CoreText 'T_WELCOME_TITLE')) + '$') 1)) 'no welcome, no main window'
# its OK as the user presses it: WM_COMMAND IDOK (second-copy.ps1's reason)
Add-Type -Namespace RbspT -Name Msg -MemberDefinition '[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);'
$dlg.Refresh()
Check ([RbspT.Msg]::PostMessageW($dlg.MainWindowHandle, 0x0111, [IntPtr]1, [IntPtr]::Zero)) 'its OK pressed'
Check ($dlg.WaitForExit(15000)) 'the refused start ends'
if ($dlg.HasExited) { Check ($dlg.ExitCode -eq 1) "with exit code 1 ($($dlg.ExitCode))" }
Start-Sleep -Seconds 2
Check (@(Get-AppProcesses | Where-Object { $_.ProcessName -eq 'RB Stems Plus' }).Count -eq 0) 'no app process left (the stub ended too)'
$after = Get-SupportTree
if ($after -cne $before) {
    $b = $before -split "`n"; $a = $after -split "`n"
    foreach ($x in (Compare-Object $b $a)) { "  changed: $($x.SideIndicator) $($x.InputObject)" }
}
# nothing in app.log: its length and its bytes as before (the TEST shell logs its download base
# only after the token check, batch/1118's Program.cs)
$added = if (Test-Path -LiteralPath $alog) { (Read-AppLog $logBefore) } else { '' }
$lines = @($added -split "`r?`n" | Where-Object { $_ })
$lines | ForEach-Object { "  app.log now: $_" }
Check ($lines.Count -eq 0 -and (Get-AppLogSize) -eq $logBefore) "app.log: no line added ($logBefore bytes before, $(Get-AppLogSize) after)"
Check ((Get-LogHead $logBefore) -ceq $headBefore) 'app.log: its bytes as before (neither cut nor rewritten)'
Check ($after -ceq $before) 'nothing else written in the support folder (files and folders)'
Done
