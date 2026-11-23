# UN8: plant the point just before a staged update's swap starts. B's app and watcher
# already run. Verify the pre-render undo, then the startup flow's once-only result.
if (-not (Test-Path -LiteralPath (Join-Path $BDir 'pids.txt'))) { Skip 'the B step recorded no processes' }
$seen = @([IO.File]::ReadAllLines((Join-Path $BDir 'pids.txt')))
$refused = @(Get-RefusedOurs | Where-Object { $_.Error -eq 5 -and $seen -match "^$($_.Id) " })
Check (@($refused | Where-Object Name -eq 'RB Stems Plus').Count -ge 1) "B's recorded app is still present and refused to A"
Check (@($refused | Where-Object Name -like 'rbsp-watch-*').Count -ge 1) "B's recorded watcher is still present and refused to A"
if ($script:VmFails) { Done }
$l = Get-Layout
Start-AppQuiet
$old = $script:app
if (-not $old) { Done }
$from = (Get-Content -Raw -LiteralPath $l.Current).Trim()
$to = '99.0.1'
$next = Join-Path $l.Apps $to
$update = Join-Path $l.Support 'update'
$pending = Join-Path $update 'pending'
$result = Join-Path $update 'result'
Check ($from -match '^\d+\.\d+\.\d+$' -and $from -ne $to -and -not (Test-Path -LiteralPath $next) -and -not (Test-Path -LiteralPath $update)) 'the pending-update fixture starts with a clean update slot'
if ($script:VmFails) { Done }
$started = ([DateTimeOffset]$old.StartTime).ToUnixTimeSeconds()
Stop-App
New-Item -ItemType Directory -Path $next, $update | Out-Null
[IO.File]::WriteAllText((Join-Path $next 'staged.txt'), 'staged')
[IO.File]::WriteAllText($pending, "$from`n$to`n$($old.Id)`n$started`n", [Text.Encoding]::ASCII)
$log = Join-Path $l.Support 'Logs\app.log'
$before = (Read-Shared $log).Length
Start-AppQuiet
if (-not $script:app) { Done }
$title = Get-CoreText 'T_UPDATE_FAILED'
$arg = @('invoke', '--pid', $script:app.Id, '--title', $title, (Get-CoreText 'T_OK'), '--type', 'Button', '--timeout', '60')
Check ((Invoke-Rbui @arg) -eq 0) 'the startup UPDATE_RESULT alert showed and was acknowledged'
$after = Read-Shared $log
Check ($after.Substring($(if ($before -le $after.Length) { $before } else { 0 })) -match [regex]::Escape("the update to $to didn't finish: the new app never started; $from stays")) 'the pre-render undo ran with B still present'
Check ((Get-Content -Raw -LiteralPath $l.Current).Trim() -eq $from) 'current still selects the old version'
Check (-not (Test-Path -LiteralPath $next)) 'the staged version was removed'
Check (-not (Test-Path -LiteralPath $pending) -and -not (Test-Path -LiteralPath $result)) 'pending and the consumed result are gone'
Stop-App
Start-AppQuiet
if (-not $script:app) { Done }
$rc = Invoke-Rbui wait-window --pid $script:app.Id --title $title --timeout 12
Check ($rc -eq 3) 'the following start did not repeat the Update failed alert'
Stop-App
Done
