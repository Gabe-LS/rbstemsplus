# UN9's reboot branch (app.md 5.5 "Removing the root", J7), as SYSTEM: the bin\ worker's exe held
# open by a SYSTEM task until j7-pending ends it: read, sharing read and delete (the worker can
# still open it for DELETE and rename it), and mapped as data, which the POSIX delete of the
# worker's own file can't pass, so the uninstall takes the reboot branch (its image renamed to
# *.del and scheduled for the restart). Then UAC's prompt for that uninstall armed (Yes).
$p = Read-TestPayload
$sha12 = ([string]$p.windows.app.admin_sha256).ToLowerInvariant().Substring(0, 12)
$exe = Join-Path $CacheRoot "bin\$sha12\RB Stems Plus.exe"
if (-not (Test-Path -LiteralPath $exe)) { Check $false "the bin\ worker is there ($exe)"; Done }
$stop = Join-Path $UacDir 'hold.stop'
$log = Join-Path $UacDir 'hold.txt'
New-Item -ItemType Directory -Force $UacDir | Out-Null
Remove-Item -LiteralPath $stop, $log -Force -ErrorAction SilentlyContinue
$cmd = @"
`$f = [IO.File]::Open('$exe', 'Open', 'Read', 'Read, Delete')
`$m = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateFromFile(`$f, `$null, 0, 'Read', `$null, 'None', `$true)
`$v = `$m.CreateViewAccessor(0, 0, 'Read')
Set-Content -LiteralPath '$log' -Value "held `$(`$v.ReadByte(0))"
`$t = [Diagnostics.Stopwatch]::StartNew()
while (-not (Test-Path -LiteralPath '$stop') -and `$t.Elapsed.TotalSeconds -lt 1800) { Start-Sleep -Milliseconds 500 }
`$v.Dispose(); `$m.Dispose(); `$f.Dispose()
Add-Content -LiteralPath '$log' -Value 'released'
"@
Start-SystemScript 'hold' $cmd 1800
Check (Wait-Until { (Read-Shared $log) -match '^held' } 60) "the bin\ worker held open and mapped by a SYSTEM task ($exe)"
Start-Consent yes
Done
