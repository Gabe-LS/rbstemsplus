param([string]$Checks, [string]$MacTrace, [string]$Work)
$ErrorActionPreference = 'Stop'
. $Checks
function Assert($ok, $label) { if (-not $ok) { throw "FAIL $label" }; "PASS $label" }

$oracle = Get-W13bMacFolder $MacTrace
Assert ($oracle['prepare.nameLabel.str'] -ceq 'Sets' -and
    $oracle['prepare.info.str'] -ceq 'A folder: choose one of its playlists.' -and
    $oracle['prepare.tracksTable.rows'] -ceq '0' -and $oracle['prepare.prepareBtn.en'] -ceq '0') 'W13b selects the Sets segment from Mac A'

$s = '{ "ev":"select", "row":0, "gen":12, "table":"T_PREP_OUTLINE" }' | ConvertFrom-Json
$f = '{ "ev":"folder", "row":0, "gen":12, "open":false }' | ConvertFrom-Json
Assert ((Test-W13bCollapse @($s, $f) 0) -and (Test-W13bCollapse @($f, $s) 0)) 'PT8 accepts Windows and Mac event ordering'
$f.gen++
Assert (-not (Test-W13bCollapse @($s, $f) 0)) 'PT8 refuses a split generation'
$f.gen--
Assert (-not (Test-W13bCollapse @($f, $s, $s) 0)) 'PT8 refuses duplicate selection'

$first = @(0x000000) * 40 + @(0xFFFFFF) * 5 + @(0xC0C0C0) * 3
$second = @(0x000000) * 50 + @(0xFFFFFF) * 4 + @(0xC0C0C0) * 2
$gray = @(0x000000) * 50 + @(0x777777) * 8
Assert (Test-W13bCellInk $first $second) 'ST3 accepts the same selected-row ink in two glyph shapes'
Assert (-not (Test-W13bCellInk $first $gray)) 'ST3 refuses gray glyphs on the selected row'

# The real driver must write the next date before opening and expect the refreshed lower bound.
$clock = Join-Path $Work 'w13b-picker-day.txt'
$script:bound = '2026-12-31'
$script:open = '0'
function WaitFor($ms, $cond) {
    $key, $value = $cond -split '=', 2
    switch ($key) {
        'prepare.until.first' { $value -ceq $script:bound }
        'prepare.until.open' { $value -ceq $script:open }
        'prepare.until.days' { $value -ceq '10' }
    }
}
function Count($name) { 3 }
function PrepareInput($verb, $target) {
    if ($verb -eq 'calendar' -and $target -eq '#PART_Button') {
        $script:open = '1'
        $script:bound = DayText ([datetime]::ParseExact([IO.File]::ReadAllText($clock), 'yyyy-MM-dd',
            [Globalization.CultureInfo]::InvariantCulture).AddDays(1))
    } elseif ($verb -eq 'escape' -and $target -eq '#Tabs') { $script:open = '0' }
    else { throw "unexpected action $verb $target" }
}
function Check($ok, $label) { Assert $ok $label }
try { Invoke-W13bMidnight $clock ([datetime]::new(2026, 12, 30)) }
finally { Remove-Item -LiteralPath $clock -ErrorAction SilentlyContinue }
