# V5, as the user: each covered folder's own onnxruntime.dll is what its EXE loads. The stand-in
# host in each folder (standin-put) runs a session on the test model (harness run: it waits 10 s
# after creating the session, so its modules can be read meanwhile); then bridge.log's lines from
# its run: "bridge loaded" with the real library 1.18.x from Stems Cache's ort\ (phase 1 and 2
# for each), no refusal, no stub, no ERROR; and, where Windows lets this side read the x64
# process's modules, its onnxruntime.dll modules: the folder's own (the bridge) and ort\'s.
$l = Get-Layout
$log = Join-Path $l.Support 'Logs\bridge.log'
$ort = Join-Path $CacheRoot 'ort\onnxruntime.dll'
$model = 'C:\rbtest\mv-model\hdemucs.onnx'
$b = Get-ServedBridge
$versions = if ($VmRun.Step -eq 'v7-load') { @('7.2.19') } else { $MvVersions }
foreach ($v in $versions) {
    $d = Get-RbDirOf $v
    $exe = Join-Path $d 'rbsp-standin.exe'
    if (-not (Test-Path -LiteralPath $exe)) { Missing "the stand-in host in $d"; continue }
    Check ((Get-Sha (Get-RbOrtOf $v)) -eq $b) "rekordbox $v's own onnxruntime.dll is the bridge"
    $from = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
    $loadsBefore = @((Read-BridgeLog $log 0) -split "`n" | Where-Object { $_ -match 'bridge loaded\b' }).Count
    $p = Start-Process -FilePath $exe -ArgumentList 'run', "`"$model`"", 'a', 'sleep*6' -WorkingDirectory $env:TEMP -NoNewWindow -PassThru `
        -RedirectStandardOutput "$env:TEMP\mv-standin-$v.out" -RedirectStandardError "$env:TEMP\mv-standin-$v.err"
    $null = $p.Handle
    $mods = $null
    [void](Wait-Until {
            if ($p.HasExited) { return $true }
            try { $script:mods = @((Get-Process -Id $p.Id).Modules | Where-Object { $_.ModuleName -ieq 'onnxruntime.dll' } | ForEach-Object FileName) } catch { $script:modErr = $_.Exception.Message }
            @($script:mods).Count -ge 2 } 30)
    [void]$p.WaitForExit((Get-WaitSecs 120) * 1000)
    "stand-in for $v (pid $($p.Id)): exit $(if ($p.HasExited) { $p.ExitCode } else { 'still running' })"
    foreach ($x in @(Get-Content -LiteralPath "$env:TEMP\mv-standin-$v.out", "$env:TEMP\mv-standin-$v.err" -ErrorAction SilentlyContinue)) { "  harness: $x" }
    if ($script:mods) {
        "  modules named onnxruntime.dll: $($script:mods -join '; ')"
        Check (@($script:mods | Where-Object { $_ -ieq (Get-RbOrtOf $v) }).Count -eq 1) "the process loaded $v's own onnxruntime.dll ($(Get-RbOrtOf $v))"
        Check (@($script:mods | Where-Object { $_ -ieq $ort }).Count -eq 1) "and Stems Cache's ort\onnxruntime.dll"
    } else { "  its modules couldn't be read from here ($script:modErr): bridge.log decides" }
    $script:mods = $null
    # this stand-in's lines: everything bridge.log got while it ran (nothing else loads the bridge
    # meanwhile; the full bridge's lines carry no pid)
    $lines = @((Read-BridgeLog $log $from) -split "`n" | Where-Object { $_.Trim() })
    foreach ($x in $lines) { "  bridge.log: $($x.TrimEnd())" }
    $re = 'bridge loaded\b.*real library (\S+) from (.+?onnxruntime\.dll)'
    $line = @($lines | Where-Object { $_ -match $re }) | Select-Object -First 1
    $m = if ($line) { [regex]::Match($line, $re) } else { $null }
    $loadsAfter = @((Read-BridgeLog $log 0) -split "`n" | Where-Object { $_ -match 'bridge loaded\b' }).Count
    Check ([bool]$line -and $loadsAfter -eq $loadsBefore + 1) "bridge.log: the bridge loaded once more in the $v stand-in (pid $($p.Id); 'bridge loaded' lines $loadsBefore -> $loadsAfter)"
    Check ($m -and $m.Groups[1].Value -like '1.18.*') "the real library is 1.18.x ($(if ($m) { $m.Groups[1].Value }))"
    Check ($m -and $m.Groups[2].Value.Equals($ort, [StringComparison]::OrdinalIgnoreCase)) "from Stems Cache's ort\ ($(if ($m) { $m.Groups[2].Value }))"
    Check (@($lines | Where-Object { $_ -match 'REFUSED|gets the stub OrtApi|\bERROR\b|under the loader lock' }).Count -eq 0) 'no refusal, no stub, no ERROR, phase 1 outside the loader lock'
}
Done
