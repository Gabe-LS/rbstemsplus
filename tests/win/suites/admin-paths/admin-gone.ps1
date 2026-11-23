# N8's VM row from review audit-fixes-rr1 (AR4, AF12): the app folder's admin\ worker (the first
# elevation's copy, admin\RB Stems Plus.exe) removed, as a damaged or pruned install leaves it;
# the app's own worker stays in Stems Cache's bin\ (the first install put it there). The later
# steps: Install stops before any elevation; Restore and Uninstall elevate the bin\ worker.
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
Stop-App
$ver = (Get-Content -Raw -LiteralPath $l.Current).Trim()
$admin = Join-Path (Join-Path $l.Apps $ver) 'admin'
Check (Test-Path -LiteralPath (Join-Path $admin 'RB Stems Plus.exe')) "the app folder's worker is there ($admin)"
Remove-Item -LiteralPath $admin -Recurse -Force
Check (-not (Test-Path -LiteralPath $admin)) 'and removed'
$own = ([string](Read-TestPayload).windows.app.admin_sha256).ToLowerInvariant()
$bin = Join-Path $CacheRoot ('bin\' + $own.Substring(0, 12) + '\RB Stems Plus.exe')
Check (Test-Path -LiteralPath $bin) "the app's own worker is in bin\ ($bin)"
Done
