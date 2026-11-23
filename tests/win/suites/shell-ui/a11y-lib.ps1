# Only an exact, newly created AUMID can be attributed to the fixture's notification.
function Test-NoticeRecord([string[]]$before, [string[]]$after, [string]$aumid) {
    return ($before -notcontains $aumid) -and ($after -contains $aumid)
}

function Get-NoticeRecords([string]$root) {
    if (Test-Path -LiteralPath $root) {
        Get-ChildItem -LiteralPath $root | ForEach-Object { $_.PSChildName }
    }
}

function Test-A11yNode([string]$tree, [string]$type, [string]$name, [string]$help = '') {
    $pattern = '(?m)^\s*' + [regex]::Escape("$type `"$name`"") + ' .*pid=\d+'
    if ($help) { $pattern += [regex]::Escape(" help=`"$help`"") }
    return $tree -cmatch ($pattern + '[^\r\n]*$')
}

function Start-RbuiCapture([string]$dir, [string]$name, [string]$arguments) {
    $exe = if (Test-Path -LiteralPath $Rbui) { $Rbui } else { Need 'vm/uia/rbui/rbui.exe' 'UIA capture' }
    $path = Join-Path $dir "$name.txt"
    $ready = Join-Path $dir "$name.ready"
    $p = Start-Process -FilePath $exe -ArgumentList "$arguments --ready `"$ready`"" -PassThru -NoNewWindow `
        -RedirectStandardOutput $path -RedirectStandardError (Join-Path $dir "$name.err")
    $null = $p.Handle   # Windows PowerShell needs the open handle to read ExitCode after exit.
    return [pscustomobject]@{ Process = $p; Path = $path; Ready = $ready; ErrorPath = (Join-Path $dir "$name.err") }
}

function Complete-NoticeCheck($capture, $sender, [string]$settings, [string]$sentinel,
                              [string[]]$before, [string]$aumid, [string]$wintest) {
    $finished = $capture.Process.WaitForExit(50000)
    Check $finished 'the notification observer finished'
    $code = if ($finished) { $capture.Process.ExitCode }
    $alive = -not $sender.HasExited
    $exitResult = if ($null -eq $code) { 'unavailable' } else { $code }
    "notification observer exit: $exitResult, sender alive: $alive"
    Check ($code -eq 0 -and $alive) 'the banner and tray icon appeared and disappeared while the sender stayed alive'

    $after = @(Get-NoticeRecords $settings)
    "notification settings added: $(@($after | Where-Object { $before -notcontains $_ }) -join ', ')"
    $owned = Test-NoticeRecord $before $after $aumid
    Check $owned "the notification created the exact $aumid record"
    if (-not $owned) { return }
    Check ((Invoke-Exe $wintest @('--balloon-cleanup')) -eq 0) 'life_records_remove returned success'
    $final = @(Get-NoticeRecords $settings)
    Check ($final -notcontains $aumid) 'the exact notification record is gone'
    Check ((Get-ItemProperty -LiteralPath $sentinel).Q13Control -eq 'intact') 'the unrelated control record stayed intact'
    Check (@($after | Where-Object { $_ -ne $aumid -and $final -notcontains $_ }).Count -eq 0) 'no other notification records disappeared during cleanup'
}

function Read-RbuiCapture($capture) {
    Get-Content -Raw -LiteralPath $capture.Path -Encoding UTF8
}

function Stop-RbuiCapture($capture) {
    if ($capture) {
        if (-not $capture.Process.HasExited) { $capture.Process.Kill(); [void]$capture.Process.WaitForExit(5000) }
        [Console]::WriteLine((Read-RbuiCapture $capture))
        [Console]::WriteLine((Get-Content -Raw -LiteralPath $capture.ErrorPath -Encoding UTF8))
        $capture.Process.Dispose()
    }
}
