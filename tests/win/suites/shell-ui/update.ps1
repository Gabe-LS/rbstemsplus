# WC6/WC7. Each delay gets a fresh a -> new swap, started from the real pin.
$l = Get-Layout
Expect ($VmRun.Step -match '^update-(native|x64)-(0|500|1000|2000)$') 'the update step names its stub variant and sweep delay'
$variant = $Matches[1]
$delay = [int]$Matches[2]
$native = @(Get-TestArchs)[0]
$arch = if ($variant -eq 'x64') { 'x64' } else { $native }
$old = (Get-Content -Raw -LiteralPath $l.Current).Trim()
$p = Get-Content -Raw -LiteralPath (Share 'dist-test/new/payload.json') | ConvertFrom-Json
$new = $p.windows.app.version
$a = Get-Content -Raw -LiteralPath (Share 'dist-test/a/payload.json') | ConvertFrom-Json
Expect ($old -eq $a.windows.app.version -and [version]$new -gt [version]$old) "an actual $old -> $new update will be offered"
Test-PinnedStub $l.Stub
$saved = Join-Path $l.Support 'payload\payload.json'
(Get-Item -LiteralPath $saved).LastWriteTime = (Get-Date).AddDays(-2)
Open-ShellPin
$app = $script:ShellApp
$app.Refresh()
$hwnd = $app.MainWindowHandle.ToInt64()
Expect ($hwnd -ne 0) 'recorded the old main window before opening the update dialog'
$notice = (Get-CoreText 'T_UPDATE_NOTICE').Replace('{version}', $new)
Expect (Find-UiText $app.Id ([regex]::Escape($notice)) 120) "the pin-started app offers $new"
Use-ShellUi menu --pid $app.Id (Get-MenuText 'T_WIN_MENU_APP') (Get-MenuText 'T_MENU_UPDATE') --timeout 30
$log = Join-Path $l.Support 'Logs\app.log'
$before = @(Get-Content -LiteralPath $log).Count
Use-ShellUi on-close $app.Id then taskbar click $ShellName --hwnd $hwnd --confirm (Get-CoreText 'T_UPDATE') --delay-ms $delay --timeout 200
Expect (Wait-Until { (Get-Content -Raw -LiteralPath $l.Current).Trim() -eq $new } 180) "the racing pin launch did not undo the swap: current is $new"
Wait-ShellApp
$updated = $script:ShellApp
Expect ($updated.Id -ne $app.Id -and $app.HasExited) "the old app exited and the new app has its own window (pid $($updated.Id))"
$lines = @(Get-Content -LiteralPath $log | Select-Object -Skip $before)
$outcome = @($lines -match 'shell: a copy is already open: it comes forward|shell: a copy is already open and shows no window|update: .+ still runs(?: from .+)?: this start leaves it alone')
$outcome | ForEach-Object { "pin race: $_" }
Expect ($outcome.Count -eq 1) 'the competing launch handed off, reported Already open, or deferred to the swap'
if ($outcome[0] -match 'shows no window') {
    $other = @(Get-AppProcesses | Where-Object {
        $_.ProcessName -eq $ShellName -and $_.Id -ne $updated.Id -and $_.MainWindowHandle -ne [IntPtr]::Zero
    })
    Expect ($other.Count -eq 1 -and (Find-UiText $other[0].Id ([regex]::Escape((Get-CoreText 'T_ALREADY_OPEN'))) 5)) 'the competing launch displayed Already open'
    Use-ShellUi invoke --pid $other[0].Id 'OK' --type Button --timeout 10
    Expect ($other[0].WaitForExit(10000)) 'the competing launch exited'
}
Test-ShellGroup
$oldDir = Join-Path $l.Apps $old
$oldGone = Wait-Until { -not (Test-Path -LiteralPath $oldDir) } 90
if (-not $oldGone) {
    Get-AppProcesses | Where-Object { $_.Path -and $_.Path.StartsWith("$oldDir\", [StringComparison]::OrdinalIgnoreCase) } |
        ForEach-Object { "still running from ${old}: $($_.Id) $($_.Path)" }
    Get-ChildItem -LiteralPath $oldDir -Recurse -Force -ErrorAction SilentlyContinue | Select-Object -First 15 | ForEach-Object { "left in ${old}: $($_.FullName)" }
    @(Get-Content -LiteralPath $log | Select-Object -Skip $before) | Select-Object -Last 25 | ForEach-Object { "app.log: $_" }
}
Check $oldGone "the old version folder $old was removed"
Check ((Get-ItemProperty -LiteralPath $ShellEntry).DisplayVersion -eq $p.payload_version) "Installed apps names $($p.payload_version)"
Check (Wait-Until { @(Get-AppProcesses | Where-Object { $_.ProcessName -like 'rbsp-watch-*' -and $_.Path.StartsWith((Join-Path $l.Apps "$new\"), [StringComparison]::OrdinalIgnoreCase) }).Count -eq 1 } 60) 'one watcher runs from the new version'
foreach ($file in 'pending', 'result') {
    Check (-not (Test-Path -LiteralPath (Join-Path $l.Support "update\$file"))) "the update left no $file record"
}
$built = Join-Path $l.Apps "$new\stub\$native\$ShellName.exe"
Check ((Get-FileHash -LiteralPath $l.Stub).Hash -eq (Get-FileHash -LiteralPath $built).Hash) 'the update installed the new native stub'
Close-ShellApp $updated
Select-ShellStub $l $new $arch
Test-PinnedStub $l.Stub
Open-ShellPin
Close-ShellApp $script:ShellApp
Use-ShellUi taskbar unpin $ShellName --timeout 20
Use-ShellUi taskbar count $ShellName --timeout 15
Expect ($script:ExeOut -match '(?m) taskbar: count=0 name=RB Stems Plus\r?$') 'the finished lap left no pin for the next lap'
Assert-ShellDesktop
[void](Start-Process -FilePath $l.Stub -ArgumentList '--uninstall')
Wait-ShellApp
Press-UninstallDialogs $script:ShellApp
Test-UninstallGone
Done
