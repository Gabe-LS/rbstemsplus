# Shared by the W13b guest and its host test. The A trace selects the same folder directly.
function Get-W13bMacFolder([string]$Trace) {
    $keys = @{
        'prepare.nameLabel|setStringValue' = 'prepare.nameLabel.str'
        'prepare.info|setAttributedStringValue' = 'prepare.info.str'
        'prepare.tracksTable|reloadData' = 'prepare.tracksTable.rows'
        'prepare.prepareBtn|setEnabled' = 'prepare.prepareBtn.en'
    }
    $selected = $false
    $fields = @{}
    foreach ($line in [IO.File]::ReadLines($Trace)) {
        $row = $line | ConvertFrom-Json
        if ($row.e -ceq 'cmd') {
            if ($selected) { break }
            $selected = $row.v -ceq 'select 1'
        }
        if ($selected -and $row.k -ceq 'set') {
            $key = "$($row.f)|$($row.s)"
            if ($keys.ContainsKey($key)) { $fields[$keys[$key]] = $row.v }
        }
    }
    if ($fields.Count -ne $keys.Count) { throw 'Mac A trace has no complete folder selection' }
    return $fields
}

# Mac and WPF send the pair in different orders. Compare the table, row and generation.
function Test-W13bOutlineSelection($Events, [int]$Row) {
    return ($Events.Count -eq 1 -and $Events[0].ev -ceq 'select' -and
            $Events[0].table -ceq 'T_PREP_OUTLINE' -and $Events[0].row -eq $Row -and
            $null -ne $Events[0].gen)
}
function Test-W13bCollapse($Events, [int]$Row) {
    $s = @($Events | Where-Object ev -CEQ 'select')
    $f = @($Events | Where-Object ev -CEQ 'folder')
    return ((Test-W13bOutlineSelection $s $Row) -and $f.Count -eq 1 -and
            $f[0].row -eq $Row -and $f[0].open -is [bool] -and -not $f[0].open -and
            $f[0].gen -eq $s[0].gen)
}

# For a cell interior on a solid high-contrast background, find the solid glyph pixels.
# Ignore isolated fringes, and choose the color furthest from the majority background.
function Get-W13bCellInk([int[]]$Pixels) {
    $colors = @{}
    foreach ($p in $Pixels) { $colors[$p]++ }
    $bg = $colors.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1
    if (-not $bg -or $bg.Value -le $Pixels.Count / 2) { return $null }
    $best = 0
    $foreground = $null
    foreach ($color in $colors.GetEnumerator()) {
        if ($color.Key -eq $bg.Key -or $color.Value -lt 3) { continue }
        $distance = 0
        foreach ($shift in 0, 8, 16) {
            $delta = (($color.Key -shr $shift) -band 255) - (($bg.Key -shr $shift) -band 255)
            $distance += $delta * $delta
        }
        if ($distance -gt $best) { $best = $distance; $foreground = $color.Key }
    }
    if ($null -eq $foreground) { return $null }
    return @{ Background = $bg.Key; Foreground = $foreground }
}
function Test-W13bCellInk([int[]]$Reference, [int[]]$Secondary) {
    $a = Get-W13bCellInk $Reference
    $b = Get-W13bCellInk $Secondary
    return ($null -ne $a -and $null -ne $b -and $a.Background -eq $b.Background -and $a.Foreground -eq $b.Foreground)
}

function DayText([datetime]$Day) { $Day.ToString('yyyy-MM-dd', [Globalization.CultureInfo]::InvariantCulture) }
function Write-PickerDay([string]$Path, [datetime]$Day) { [IO.File]::WriteAllText($Path, (DayText $Day)) }

function Invoke-W13bMidnight([string]$ClockPath, [datetime]$ClockDay) {
    $before = DayText ($ClockDay.AddDays(1))
    $after = DayText ($ClockDay.AddDays(2))
    Check (WaitFor 3000 "prepare.until.first=$before") "calendar first selectable date before midnight: $before"
    $u0 = Count 'sent:until'
    Write-PickerDay $ClockPath ($ClockDay.AddDays(1))
    Check (WaitFor 100 "prepare.until.first=$before") 'without a render, the old range remains before opening'
    PrepareInput 'calendar' '#PART_Button'
    Check ((WaitFor 3000 'prepare.until.open=1') -and (WaitFor 3000 "prepare.until.first=$after")) "calendar opens at the new tomorrow: $after"
    PrepareInput 'escape' '#Tabs'
    Check ((WaitFor 3000 'prepare.until.open=0') -and (WaitFor 3000 'prepare.until.days=10') -and
        $u0 -ge 0 -and (Count 'sent:until') -eq $u0) 'opening the calendar preserves the chosen day and sends no until event'
}
