# match-lib.ps1 (dot-source it): which corpus row an OCR reading of deck 1's title is. Pure
# PowerShell (no Windows API): pc-session-test.sh runs it on the Mac with pwsh.
# An OCR region (x, y, w, h) calibrated on the 3840-pixel-wide screen at 200 % scaled to a
# screenshot WIDTH pixels wide: the same logical layout (1920x1080 at 100 % halves it, as on
# the PC on 2026-11-19).
function Scale-Region([int]$width, [int[]]$r) {
    if ($width -le 0 -or $r.Count -ne 4) { throw 'Scale-Region: a width and x, y, w, h' }
    $f = $width / 3840
    return @([int][math]::Floor($r[0] * $f), [int][math]::Floor($r[1] * $f), [int][math]::Ceiling($r[2] * $f), [int][math]::Ceiling($r[3] * $f))
}
# compare a recognised title with a corpus name: lower case, only letters and digits
function Norm([string]$s) { ($s.ToLowerInvariant() -replace '[^a-z0-9]', '') }
function Dist([string]$a, [string]$b) {
    $d = New-Object 'int[,]' ($a.Length + 1), ($b.Length + 1)
    for ($x = 0; $x -le $a.Length; $x++) { $d[$x, 0] = $x }; for ($y = 0; $y -le $b.Length; $y++) { $d[0, $y] = $y }
    for ($x = 1; $x -le $a.Length; $x++) { for ($y = 1; $y -le $b.Length; $y++) {
        $c = if ($a[$x - 1] -eq $b[$y - 1]) { 0 } else { 1 }
        # Windows PowerShell 5.1 can't parse a 2-D index inside a method's arguments: temporaries
        $del = $d[($x - 1), $y] + 1; $ins = $d[$x, ($y - 1)] + 1; $sub = $d[($x - 1), ($y - 1)] + $c
        $d[$x, $y] = [math]::Min([math]::Min($del, $ins), $sub) } }
    return $d[$a.Length, $b.Length]
}
# the row (index into $rows) a title reads as: the closest name or alias within 3 edits and at
# least 2 closer than any other row (an OCR slip such as "0-mp3-22k" for "w15-mp3-22k" still
# points at one row only); -1: none of ours. $aliases: row name -> titles deck 1 shows instead
# OCR's usual confusions folded before the edit distance: i and l read for 1, o for 0 (deck 1 read
# "II-flac-24-96k" for 11-flac-24-96k on the 1920x1080 screen, 2026-11-19); both sides are folded
function Fold([string]$s) { $s.Replace('i', '1').Replace('l', '1').Replace('o', '0') }
function Find-Row([string]$t, [string[]]$rows, [hashtable]$aliases) {
    $k = Fold (Norm $t); if (-not $k) { return -1 }
    $ds = @(for ($j = 0; $j -lt $rows.Count; $j++) {
        $names = @($aliases[$rows[$j]] | Where-Object { $_ })
        if (-not $names.Count) { $names = @($rows[$j]) }
        [pscustomobject]@{ j = $j; e = (@($names | ForEach-Object { Dist $k (Fold (Norm $_)) }) | Measure-Object -Minimum).Minimum } }) | Sort-Object e
    if (($ds[0].e -eq 0 -and ($ds.Count -lt 2 -or $ds[1].e -ge 1)) -or ($ds[0].e -le 3 -and ($ds.Count -lt 2 -or $ds[1].e -ge $ds[0].e + 2))) { return [int]$ds[0].j } else { return -1 }
}
# the row most readings (one per OCR scale) point at: at least 2 votes, more than any other row; -1: no clear answer
function Vote-Row($ts, [string[]]$rows, [hashtable]$aliases) {
    $v = @{}; foreach ($x in $ts) { $j = Find-Row $x $rows $aliases; if ($j -ge 0) { $v[$j] = 1 + [int]$v[$j] } }
    $s = @($v.GetEnumerator() | Sort-Object Value -Descending)
    if ($s.Count -ge 1 -and $s[0].Value -ge 2 -and ($s.Count -lt 2 -or $s[1].Value -lt $s[0].Value)) { return [int]$s[0].Key } else { return -1 }
}
# the bridge's log lines of one load, counted: misses, hits, refused hits (a damaged entry, served as
# a miss: bridge.c's "hit  KEY refused:"), near serves ("near KEY for KEY:")
function Count-Bridge([string[]]$new) {
    $new = @($new)
    return @{ miss = @($new -match '^\S+ \S+  miss ').Count; hit = @($new -match '^\S+ \S+  hit  \S+ \(\d+ samples').Count
              refused = @($new -match '^\S+ \S+  hit  \S+ refused:').Count; near = @($new -match '^\S+ \S+  near \S+ for ').Count }
}
