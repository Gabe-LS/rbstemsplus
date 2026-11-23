# N10 (review RD4's follow-up: the first real run of the native r1_dismiss step and of the named
# mutex): "Don't Ask Again for This Version" pressed while this step holds the answer lock
# (Local\io.github.rbstemsplus.answer, the watcher's `remember` takes the same one) for 9 s: the
# app's writer waits for that mutex, at most DISMISSED_LOCK_MS (5 s), then writes anyway (app.md
# section 5.2: a lock not had within 5 s doesn't stop the answer), so r1:7.2.19 shows in the
# support folder's dismissed about 5 s after the click, while the lock is still held: never at
# once (a writer on another lock, or none), never only after the release. The line already there
# (planted) is kept. The button goes; R1's line and Put Back stay (a dismissed version only stops
# the watcher opening the app); nothing in app.log says the write failed. What this shows is the
# writer's bounded wait on the shared mutex (a writer that skips the lock, waits on another one or
# waits for the release fails it); an early release isn't tried (it needs a second R1 offer).
$l = Get-Layout
$app = Get-AppWindow
if (-not $app) { Skip 'the app isn''t open (r1-offer failed: its log)' }
$script:app = $app
$dis = Join-Path $l.Support 'dismissed'
$has = { param($re) (Test-Path -LiteralPath $dis) -and @((Get-Content -LiteralPath $dis -ErrorAction SilentlyContinue) | Where-Object { $_ -match $re }).Count -gt 0 }
if (Test-Path -LiteralPath $dis) { "dismissed before: $((Get-Content -Raw -LiteralPath $dis) -replace "`r?`n", ' | ')" }
[IO.File]::AppendAllText($dis, "rbtest:kept-line`n", (New-Object Text.UTF8Encoding $false))
Check (-not (& $has '^r1:')) 'no r1: line in dismissed before the click'
$from = Get-AppLogSize
$m = New-Object Threading.Mutex($false, $WtAnswerLock)
$held = $false
try { $held = $m.WaitOne(2000) } catch [Threading.AbandonedMutexException] { $held = $true }
Check $held "this step holds the answer lock ($WtAnswerLock)"
$dont = Get-CoreText 'T_W_DONT_ASK'
Check (Invoke-UiNamed $app.Id $dont 30 'Button') "$dont pressed"
$t = [Diagnostics.Stopwatch]::StartNew()
$at = $null
while ($t.Elapsed.TotalSeconds -lt 9) {
    if ($null -eq $at -and (& $has '^r1:7\.2\.19$')) { $at = $t.Elapsed.TotalSeconds }
    Start-Sleep -Milliseconds 100
}
if ($held) { $m.ReleaseMutex() }
$m.Dispose()
if ($null -eq $at -and (Wait-Until { & $has '^r1:7\.2\.19$' } 10)) { $at = $t.Elapsed.TotalSeconds }
"dismissed after: $((Get-Content -Raw -LiteralPath $dis -ErrorAction SilentlyContinue) -replace "`r?`n", ' | ')"
"the r1: line showed $(if ($null -ne $at) { '{0:N1} s' -f $at } else { 'never' }) after the click (the lock released at 9 s)"
Check ($null -ne $at) 'the click''s line is written: r1:7.2.19'
Check ($null -ne $at -and $at -ge 4.0) 'not at once: the writer waited for the answer lock this step held'
Check ($null -ne $at -and $at -lt 8.5) 'and wrote within its 5 s bound, the lock still held'
Check (& $has '^rbtest:kept-line$') 'the line already there is kept'
Check (Wait-Until { -not (Find-UiText $app.Id ('^' + [regex]::Escape($dont) + '$') 1) } 30) 'the button goes'
Check (Find-UiText $app.Id ('^' + [regex]::Escape((Get-CoreText 'T_WIN_R1_LIVE_FOREIGN')) + '$') 5) 'R1''s line stays'
Check (Wait-UiEnabled $app.Id (Get-CoreText 'T_WIN_BTN_RESTORE') 10) 'Put Back stays'
$said = Read-AppLog $from
($said -split "`r?`n" | Where-Object { $_ }) | ForEach-Object { "app.log: $_" }
Check ($said -notmatch "couldn't write dismissed") 'app.log: no failed write'
Stop-App
Done
