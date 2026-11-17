# Install Stems Cache through the app, as the user would: the main window's Install Stems Cache,
# its confirmation's Install (core/flows.c F_INSTALL_CACHE), the download of the TEST payload's
# bridge, then the shipped worker elevated (the first install: admin\RB Stems Plus.exe from the
# app's folder puts itself into bin\, app.md 5.5), which swaps rekordbox's onnxruntime.dll for
# the bridge. Done when the app has turned Stems Cache on (rekordbox_model=1 in config.ini, the
# last thing the install writes) and rekordbox's DLL is the payload's bridge. rekordbox is closed
# first when the snapshot has it open (the install refuses while it runs). cache-files checks
# the root's files as SYSTEM.
$l = Get-Layout
$f = Read-WorkerFacts
if (-not $f) { Skip 'no facts from the prep step (it failed: its log)' }
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
$p = Read-TestPayload
$bridge = ([string]$p.windows.bridge.sha256).ToLowerInvariant()
if (@(Get-Rekordbox).Count) { Check (Close-Rekordbox 200) 'rekordbox, open in the snapshot, closed first' }
[void](Start-Process -FilePath $l.Stub -PassThru)
$script:app = $null
Check (Wait-Until { $script:app = Get-AppWindow; $null -ne $script:app } 90) 'the app opened through the stub (its window)'
if (-not $script:app) { Done }
$label = Get-CoreText 'T_BTN_INSTALL_CACHE'
Check (Wait-UiEnabled $script:app.Id $label 120) "its $label button is there and enabled (rekordbox 7.2.19 listed, an admin account)"
$t = [Diagnostics.Stopwatch]::StartNew()
Check (Invoke-UiNamed $script:app.Id $label 30 'Button') "$label pressed"
Check (Invoke-UiNamed $script:app.Id (Get-CoreText 'T_INSTALL') 60 'Button') 'its confirmation answered: Install'
$cfg = Join-Path $l.Support 'config.ini'
$on = Wait-Until { (Test-Path -LiteralPath $cfg) -and [bool](Select-String -LiteralPath $cfg -Pattern '^\s*rekordbox_model\s*=\s*1\s*$' -Quiet) } 420
Check $on "the install ended with Stems Cache on (rekordbox_model=1 in config.ini) after $([int]$t.Elapsed.TotalSeconds) s"
$live = Get-Sha $RbOrt
Check ($live -eq $bridge) "rekordbox 7.2.19's onnxruntime.dll is the TEST payload's bridge ($live)"
Check ($live -ne $f.PioneerSha) "and no longer Pioneer's ($($f.PioneerSha))"
$worker = ([string]$p.windows.app.admin_sha256).ToLowerInvariant()
$ver = (Get-Content -Raw -LiteralPath $l.Current).Trim()
$first = Join-Path (Join-Path $l.Apps $ver) 'admin\RB Stems Plus.exe'
Check ((Get-Sha $first) -eq $worker) "the app folder's first-elevation copy is the payload's worker ($first)"
Stop-App
Done
