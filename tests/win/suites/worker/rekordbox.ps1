# rekordbox started with the shipped bridge, as the user: its window up, and bridge.log's start
# line from this start (rekordbox calls OrtGetApiBase on its main thread at start-up,
# phase0-probes.md 2.1): the bridge loaded the real 1.18 library from Stems Cache's ort\ (not
# refused, not passing through, no stub given to rekordbox). Then rekordbox closed as a user
# would and waited for; its process lingers about 75 s and may end with its own crash at exit
# (phase0-probes.md 2.2), which is no failure here; one still running after that is.
$l = Get-Layout
if (-not (Read-WorkerFacts)) { Skip 'no facts from the prep step (it failed: its log)' }
$bridge = ([string](Read-TestPayload).windows.bridge.sha256).ToLowerInvariant()
if ((Get-Sha $RbOrt) -ne $bridge) { Skip "rekordbox's onnxruntime.dll isn't the bridge (cache-install failed: its log)" }
if (@(Get-Rekordbox).Count) { Check (Close-Rekordbox 150) 'a rekordbox already open closed first' }
$log = Join-Path $l.Support 'Logs\bridge.log'
$from = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
$script:rb = Start-Process -FilePath $RbExe -PassThru
$null = $script:rb.Handle
$up = Wait-Until { $script:rb.Refresh(); $script:rb.HasExited -or $script:rb.MainWindowTitle -eq 'rekordbox' } 180
Check ($up -and -not $script:rb.HasExited) "rekordbox started, its window up (pid $($script:rb.Id))"
$re = 'bridge loaded\b.*real library (\S+) from (.+?onnxruntime\.dll)'
Check (Wait-Until { (Read-BridgeLog $log $from) -match $re } 120) "bridge.log has this start's \"bridge loaded\" line ($log)"
$new = @((Read-BridgeLog $log $from) -split "`n" | Where-Object { $_ })
foreach ($x in $new) { "  bridge.log: $x" }
$line = @($new | Where-Object { $_ -match $re }) | Select-Object -First 1
$m = if ($line) { [regex]::Match($line, $re) } else { $null }
Check ($m -and $m.Groups[1].Value -like '1.18.*') "the real library is 1.18.x ($(if ($m) { $m.Groups[1].Value }))"
$want = Join-Path $CacheRoot 'ort\onnxruntime.dll'
Check ($m -and $m.Groups[2].Value.Equals($want, [StringComparison]::OrdinalIgnoreCase)) "loaded from Stems Cache's ort\ ($(if ($m) { $m.Groups[2].Value }))"
Check (-not ($line -match 'NOT 1\.18\.x|REFUSED')) 'not refused, not passing everything through'
Check (@($new | Where-Object { $_ -match 'gets the stub OrtApi|\bERROR\b' }).Count -eq 0) 'no stub for rekordbox and no ERROR line in this start'
if ($line -match 'ENTRY ONLY') { "note: this bridge is entry-only (it caches nothing): cache-hit can't pass with it" }
Check (Close-Rekordbox 200) 'rekordbox closed (its window, then its process) within 200 s'
Done
