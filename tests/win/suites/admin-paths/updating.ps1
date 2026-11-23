# N12 item 14 (review AF8): Cancel at the "rekordbox updating" prompt keeps Stems Cache's choice.
# With rekordbox's version folder moved aside (rb-moved), Uninstall Stems Cache > Uninstall asks
# "rekordbox missing" (remove the copy of rekordbox's files: Cancel, Remove); before Remove is
# pressed, rekordbox's updater runs (a copy of PING.EXE named "Upmgr rekordbox.exe", the image
# name compat.c's process list counts as updating: nothing of rekordbox runs); the remove-root-
# folder run's rekordbox check then asks "rekordbox updating" (Try Again, Cancel): Cancel. No UAC
# prompt; app.log "rekordbox is open or updating: stopped" and "stopped: Stems Cache's choice
# kept"; the support folder's installed record as before, Stems Cache still on, its root there.
$l = Get-Layout
$f = Read-WorkerFacts
if (-not $f) { Skip 'no facts from the prep step (it failed: its log)' }
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
if (Test-Path -LiteralPath $RbExe) { Skip "rekordbox's folder wasn't moved aside (rb-moved: its log)" }
if (-not (Test-CacheOn)) { Skip 'Stems Cache is not on (cache-install failed: its log)' }
$rec = Join-Path $l.Support 'installed'
Check (Test-Path -LiteralPath $rec) "the installed record is there ($rec)"
$before = if (Test-Path -LiteralPath $rec) { Get-Content -Raw -LiteralPath $rec } else { '' }
"installed before: $($before -replace "`r?`n", ' | ')"
Check ([bool]$before.Trim()) 'and it holds the choice (not empty)'
$from = Get-AppLogSize
Open-App
if (-not $script:app) { Done }
Invoke-AppButton 'T_BTN_UNINSTALL_CACHE' 'T_UNINSTALL'
if (-not $script:Pressed) { Done }
$missing = Get-CoreText 'T_ROOT_MISSING_TITLE'
$asked = Find-UiText $script:app.Id ('^' + [regex]::Escape($missing) + '$') 90
Check $asked "the app asks: $missing (remove the copy of rekordbox's files)"
if (-not $asked) { Show-AppState; Stop-App; Done }
$dir = Join-Path $env:TEMP 'rbtest-upd'
New-Item -ItemType Directory -Force $dir | Out-Null
$upd = Join-Path $dir 'Upmgr rekordbox.exe'
Copy-Item -LiteralPath "$env:WINDIR\System32\PING.EXE" -Destination $upd -Force
$u = Start-Process -FilePath $upd -ArgumentList '-n', '200', '127.0.0.1' -WindowStyle Hidden -PassThru
Check (-not $u.HasExited) "rekordbox's updater runs (a PING.EXE copy named Upmgr rekordbox.exe, pid $($u.Id))"
Check (Invoke-UiNamed $script:app.Id (Get-CoreText 'T_REMOVE') 30 'Button') 'Remove pressed'
$upTitle = Get-CoreText 'T_ROOT_UPDATING_TITLE'
$shown = Find-UiText $script:app.Id ('^' + [regex]::Escape($upTitle) + '$') 60
Check $shown "the app asks: $upTitle"
Check (-not (Test-ConsentShown)) 'no UAC prompt meanwhile'
if ($shown) { Check (Invoke-UiNamed $script:app.Id (Get-CoreText 'T_CANCEL') 30 'Button') 'Cancel pressed' }
Check (Wait-Until { -not (Find-UiText $script:app.Id ('^' + [regex]::Escape($upTitle) + '$') 1) } 30) 'the prompt is gone'
Start-Sleep -Seconds 3
$said = Read-AppLog $from
"the app's log meanwhile:"
foreach ($x in ($said -split "`r?`n")) { if ($x) { "  $x" } }
Check ($said -match 'rekordbox is open or updating: stopped') 'app.log: rekordbox is open or updating: stopped'
Check ($said -match "stopped: Stems Cache's choice kept") "app.log: stopped: Stems Cache's choice kept"
$after = if (Test-Path -LiteralPath $rec) { Get-Content -Raw -LiteralPath $rec } else { '' }
"installed after: $($after -replace "`r?`n", ' | ')"
Check ($after -ceq $before) 'the installed record (the choice) as before'
Check (Test-CacheOn) 'Stems Cache is still on (rekordbox_model=1)'
Check (Test-Path -LiteralPath $CacheRoot) "Stems Cache's root is still there ($CacheRoot)"
Stop-Process -Id $u.Id -Force -ErrorAction SilentlyContinue
Stop-App
Done
