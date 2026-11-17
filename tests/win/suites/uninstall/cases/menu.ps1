# Round 1: the complete uninstall from the app's menu (RB Stems Plus > Uninstall RB Stems Plus...)
# after a quiet start (welcomed already), with plant.ps1's cases in place and the app started with
# TEMP and TMP set to the odd folder (! % ^). Then: everything gone as uninstall/complete.ps1
# checks; the junction deleted as itself, its target and its file kept; the after-exit temp folder
# in the odd %TEMP% gone within a minute (no RunOnce fallback exists for such a path, so it must
# go); the planted ping.cmd never ran, though the copy's exe was held open (FileShare.Read, no
# delete) from when the copy ran until about 5 s after its cmd loop started, so the loop's first
# passes failed and reached its PING.EXE (review WA6); and a control: a bare ping from cmd /d /c in the odd folder does run the
# planted one. The plain UsageLogs entry is gone (review WA9).
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
$odd = Join-Path $l.Local 'Temp\rb!%^t'
$target = Join-Path $CaseDir 'junction-target'
if (-not (Test-Path -LiteralPath $odd) -or -not (Test-Path -LiteralPath (Join-Path $target 'keep.txt'))) { Skip "plant's cases aren't there (it failed: its log)" }
Stop-App
$env:TEMP = $odd; $env:TMP = $odd   # inherited by the stub, the app and its after-exit copy
Start-AppQuiet
if (-not $script:app) { Done }
$app = $script:app
$usage = Join-Path $l.Local 'Microsoft\CLR_v4.0\UsageLogs\RB Stems Plus.exe.log'
Check (Wait-Until { Test-Path -LiteralPath $usage -PathType Leaf } 30) "the plain UsageLogs entry is there ($usage)"
# the after-exit copy's exe held open from another thread, once a process runs from it (the copy
# waits for the app to quit, then deletes the app; only then does its cmd loop start), until about
# 5 s after a cmd.exe naming the copy's folder runs
$hold = [hashtable]::Synchronized(@{ Held = ''; Loop = $false; Stop = $false })
$holder = [PowerShell]::Create()
[void]$holder.AddScript({
    param($odd, $hold, $secs)
    $t = [Diagnostics.Stopwatch]::StartNew()
    while (-not $hold.Stop -and $t.Elapsed.TotalSeconds -lt $secs) {
        foreach ($d in @(Get-ChildItem -LiteralPath $odd -Directory -Force -Filter 'rbsp-uninstall-*' -ErrorAction SilentlyContinue)) {
            $exe = Join-Path $d.FullName 'RB Stems Plus.exe'
            if (@(Get-Process -Name 'RB Stems Plus' -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }).Count -eq 0) { continue }
            $f = $null
            try { $f = [IO.File]::Open($exe, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read) } catch { }
            if (-not $f) { continue }
            try {
                $hold.Held = $exe
                while (-not $hold.Loop -and -not $hold.Stop -and $t.Elapsed.TotalSeconds -lt $secs) {
                    $hold.Loop = @(Get-CimInstance Win32_Process -Filter "Name='cmd.exe'" -ErrorAction SilentlyContinue | Where-Object { [string]$_.CommandLine -like "*$($d.Name)*" }).Count -gt 0
                    if (-not $hold.Loop) { Start-Sleep -Milliseconds 200 }
                }
                Start-Sleep -Seconds 5
            } finally { $f.Dispose() }
            return
        }
        Start-Sleep -Milliseconds 100
    }
}).AddArgument($odd).AddArgument($hold).AddArgument((Get-WaitSecs 300))
$holding = $holder.BeginInvoke()
Invoke-MenuUninstall $app
Complete-Uninstall $app
Test-UninstallGone
Check (Test-Path -LiteralPath (Join-Path $target 'keep.txt')) "the junction's target and its file kept ($target)"
$copies = Get-AfterExitCopies $odd
"after-exit copies in the odd TEMP: $($copies -join ', ')"
Check (Wait-Until { (Get-AfterExitCopies $odd).Count -eq 0 } 90) 'the after-exit temp folder in the odd %TEMP% is gone (EnableExtensions=0 in HKCU)'
$hold.Stop = $true
[void]$holder.EndInvoke($holding); $holder.Dispose()
Check ([bool]$hold.Held -and $hold.Loop) "the copy's exe held open while it ran ($($hold.Held)) until about 5 s into its cmd loop: the loop's first passes failed"
Check (@(Get-CleanupRunOnce).Count -eq 0) 'and no RunOnce cleanup value'
$marker = Join-Path $CaseDir 'ping-ran.txt'
Check (-not (Test-Path -LiteralPath $marker)) 'the planted %TEMP%\ping.cmd never ran'
Check (Wait-Until { -not (Test-Path -LiteralPath $usage) } 10) "the plain UsageLogs entry is gone ($usage)"
# the control: a bare ping from cmd in the odd folder finds the planted ping.cmd and writes the marker
$p = Start-Process -FilePath "$env:WINDIR\System32\cmd.exe" -ArgumentList '/d', '/c', 'ping -n 1 127.0.0.1' -WorkingDirectory $odd -PassThru -WindowStyle Hidden
Check ($p.WaitForExit(30000) -and (Test-Path -LiteralPath $marker)) "the control: a bare ping from cmd /d /c in $odd runs the planted ping.cmd (its marker written)"
Remove-Item -LiteralPath $marker -Force -ErrorAction SilentlyContinue
Done
