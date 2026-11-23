# N1.4-.6: the real set from helper-path, plus small on-disk parser boundary fixtures.
$l = Get-Layout
$prepared = Join-Path $l.Local "rbtest-prepared-$($VmRun.Run).json"
if (-not (Test-Path -LiteralPath $prepared)) { Skip 'the real Prepare run did not leave a kept set' }
$state = Get-Content -LiteralPath $prepared -Raw | ConvertFrom-Json
$set = [string]$state.Set
if ($set -notmatch '^[A-Za-z0-9_-]{1,64}$') { throw 'invalid fixture set id' }
$kept = Join-Path $l.Support 'kept'
$setFile = Join-Path $kept "$set.kept"
$manifest = Join-Path $kept "$set.manifest"
$expired = 'zzz-rbsp-expired'
$malformed = 'zzz-rbsp-undated'
$first = '000-rbsp-sort'
$planted = @()
try {
    $setCount = @(Get-Content -LiteralPath $manifest -Encoding UTF8 | Where-Object { $_.StartsWith('file ') }).Count
    Expect ($setCount -gt 0) 'the PF4 playlist produced manifest entries'
    foreach ($f in @($expired, $malformed, $first)) {
        $p = Join-Path $kept "$f.kept"
        Expect (-not (Test-Path -LiteralPath $p)) "fixture path is unused: $p"
        $planted += $p
        $value = switch ($f) {
            $expired { "rbstemsplus-kept 1`nuntil 1`n" }
            $malformed { "unrecognised format`n" }
            $first { "rbstemsplus-kept 1`nuntil $($state.Until)`n" }
        }
        [IO.File]::WriteAllText($p, $value)
    }
    Start-PrepareApp
    $appPid = $script:ShellApp.Id
    Use-ShellUi invoke --pid $appPid (Get-CoreText 'T_TAB_STATUS') --type TabItem --timeout 20
    $long = (Get-PrepareDate $state.Until).ToLongDateString()
    $row = 'RBSP PF4, ' + $setCount + ', ' + $long + ', '
    Expect (Wait-Until {
        $rc = Invoke-Rbui read --pid $appPid --timeout 2
        $script:StatusRows = @(Get-PrepareStatusRows $script:ExeOut)
        $rc -eq 0 -and @($script:StatusRows | Where-Object { $_.StartsWith($row) }).Count -eq 1
    } 30) 'the Status row names the playlist, its file count and the locale long date'

    $unnamed = Get-CoreText 'T_PREP_PLAYLIST_ID'
    $firstName = $unnamed.Replace('{name}', $first)
    $expiredName = $unnamed.Replace('{name}', $expired)
    $undatedName = $unnamed.Replace('{name}', $malformed)
    $dash = Get-CoreText 'T_DASH'
    $undated = "$undatedName, $dash, $dash, "
    Check (@($script:StatusRows | Where-Object { $_.StartsWith($undated) }).Count -eq 1) 'a malformed set has a row with no date'
    Check (@($script:StatusRows | Where-Object { $_.StartsWith("$expiredName, ") }).Count -eq 0) 'the expired set has no row'
    Check ($script:StatusRows.Count -eq 3 -and $script:StatusRows[0].StartsWith("$firstName, ") -and
        $script:StatusRows[1].StartsWith($row) -and $script:StatusRows[2].StartsWith($undated)) 'rows appear in kept-set id order'
    $actualRow = $script:StatusRows | Where-Object { $_.StartsWith($row) } | Select-Object -First 1
    Use-ShellUi invoke --pid $appPid $actualRow --type DataItem --timeout 15
    Use-ShellUi invoke --pid $appPid (Get-CoreText 'T_BTN_STOP_KEEPING') --type Button --timeout 15
    $title = (Get-CoreText 'T_STOP_KEEPING_TITLE').Replace('{playlist}', 'RBSP PF4')
    Use-ShellUi invoke --pid $appPid --title $title (Get-CoreText 'T_STOP_KEEPING') --type Button --timeout 15
    Check (Wait-Until { -not (Test-Path -LiteralPath $setFile) -and -not (Test-Path -LiteralPath $manifest) } 20) 'Stop Keeping removes the bridge kept file and the manifest'
    Check (Wait-Until {
        $rc = Invoke-Rbui read --pid $appPid --timeout 2
        $rows = @(Get-PrepareStatusRows $script:ExeOut)
        $rc -eq 0 -and @($rows | Where-Object { $_.StartsWith($undated) }).Count -eq 1 -and
            @($rows | Where-Object { $_.StartsWith('RBSP PF4, ') }).Count -eq 0
    } 20) 'the stopped set left the table and the unrelated row stayed'
} finally {
    Stop-App
    foreach ($p in $planted) { Remove-Item -LiteralPath $p -Force }
    Remove-Item -LiteralPath $prepared -Force
}
Done
