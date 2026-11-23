# MQ4/MT5: a 258-byte UTF-8 component must survive Clear and go on uninstall.
$fs = Get-VolFs
$v = Read-VolInfo $fs
$cjk = ([string][char]0x97f3) * 86
Check ([Text.Encoding]::UTF8.GetByteCount($cjk) -eq 258) 'the fixture name is 258 UTF-8 bytes'
foreach ($arch in Get-TestArchs) {
    $exe = Get-TestExe $arch 'winc-location-wintest.exe'
    if (-not $exe) { Missing "winc-location-wintest.exe for $arch"; continue }
    $base = "$($v.Letter):\mq4-$arch"
    $support = Join-Path $base 'support'
    $chosen = Join-Path $base 'chosen'
    New-Item -ItemType Directory -Path $chosen, $support -Force | Out-Null
    $rc = Invoke-Exe $exe @('create', $support, $chosen)
    $ok = $rc -eq 0 -and $script:ExeOut -match '(?m)^create 0\|' -and $script:ExeOut -match '(?m)^write 0$'
    Check $ok "$fs ${arch}: the native location fixture has a verified marker and config"
    if (-not $ok) { continue }
    $cache = Join-Path $chosen $CacheName
    $long = Join-Path $cache $cjk
    $stems = Join-Path $cache ('0' * 64)
    $companion = Join-Path $chosen "._$CacheName"
    [IO.File]::WriteAllText($long, 'unknown file')
    [IO.File]::WriteAllText($stems, 'cache entry')
    [IO.File]::WriteAllText($companion, 'companion')
    $rc = Invoke-Exe $exe @('clear', $support)
    Check ($rc -eq 0 -and $script:ExeOut -match '(?m)^clear 0 \|$') "$fs ${arch}: Clear returned 0"
    Check ((Test-Path -LiteralPath $long) -and (Test-Path -LiteralPath $companion) -and -not (Test-Path -LiteralPath $stems)) "$fs ${arch}: Clear kept the CJK name and companion and removed the cache entry"
    $rc = Invoke-Exe $exe @('remove', $support)
    Check ($rc -eq 0 -and $script:ExeOut -match '(?m)^remove 0 \|$') "$fs ${arch}: uninstall returned 0"
    Check (-not (Test-Path -LiteralPath $cache) -and -not (Test-Path -LiteralPath $companion)) "$fs ${arch}: uninstall removed the folder and its companion"
}
Done
