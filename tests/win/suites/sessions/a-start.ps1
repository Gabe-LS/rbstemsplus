# UN8: A's next start with B's copy running (account-b.ps1's: another account's processes under our
# image names). This token can't open them (ERROR_ACCESS_DENIED) and they never count (review GH1):
# the start's sweep (update_after_render) removes a planted leftover version folder, which it does
# only once running_under says nothing runs from it (0; an unknown -1 would keep it). The update
# undo probe (update_at_start) asks the same running_under and runs separately in a-undo.
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
$refused = @(Get-RefusedOurs)
foreach ($r in $refused) { "  refused: pid $($r.Id) $($r.Name), session $($r.Session), error $($r.Error)" }
Check (@($refused | Where-Object { $_.Name -like 'rbsp-watch-*' -and $_.Error -eq 5 }).Count -ge 1) "B's watcher is listed by name and refused to this token: ERROR_ACCESS_DENIED (5)"
Check (@($refused | Where-Object { $_.Name -eq 'RB Stems Plus' -and $_.Error -eq 5 }).Count -ge 1) "B's app too"
Stop-App
$stale = Join-Path $l.Apps '0.0.1'
New-Item -ItemType Directory -Force $stale | Out-Null
Set-Content -LiteralPath (Join-Path $stale 'leftover.txt') -Value 'planted' -Encoding ASCII
Check (Test-Path -LiteralPath $stale) "a leftover version folder planted ($stale)"
Start-AppQuiet
Check (Wait-Until { -not (Test-Path -LiteralPath $stale) } 60) "the start's sweep removed it: B's processes didn't hold it"
Stop-App
Done
