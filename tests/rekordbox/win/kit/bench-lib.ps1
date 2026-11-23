# bench-lib.ps1 (dot-source it): the DirectML bench's parsing, kept apart from the PC (tested on the
# Mac by pc-session-test.sh).
#   Get-BenchSummary LINES     a helper run's stderr (a --test-timing build): its chunks, their mean
#                              times, its execution provider and DirectML's own lines
#   Get-DeckSeconds TEXT       deck 1's elapsed time read by OCR ("-01:23.4 00:36.6"): the readout
#                              without a minus, in seconds ($null if none)
#   Get-Advance SAMPLES        deck 1 kept playing? from (wall seconds, deck seconds) pairs

# rbsp-timing chunk ... model_ns=N ort_ns=N ... chunk_ns=N; rbsp-timing track ... ep=cpu|dml;
# "DirectML: ..." lines (the provider's own: loaded, its version, or why the CPU runs it)
function Get-BenchSummary([string[]]$lines) {
    $ort = New-Object System.Collections.Generic.List[double]; $model = New-Object System.Collections.Generic.List[double]
    $chunk = New-Object System.Collections.Generic.List[double]; $eps = @{}; $dml = @()
    foreach ($l in $lines) {
        if ($l -match '^rbsp-timing chunk .*\bmodel_ns=(\d+) ort_ns=(\d+) .*\bchunk_ns=(\d+)') {
            $model.Add([double]$Matches[1] / 1e6); $ort.Add([double]$Matches[2] / 1e6); $chunk.Add([double]$Matches[3] / 1e6)
        } elseif ($l -match '^rbsp-timing track .*\bep=(cpu|dml)\b') { $eps[$Matches[1]] = $true }
        elseif ($l -match '^DirectML: ') { $dml += $l.TrimEnd() }
    }
    function Mean($x) { if ($x.Count) { [math]::Round(($x | Measure-Object -Average).Average, 1) } else { $null } }
    function Median($x) { if (-not $x.Count) { return $null }; $s = @($x | Sort-Object); [math]::Round($s[[int][math]::Floor(($s.Count - 1) / 2)], 1) }
    [pscustomobject]@{
        Chunks = $ort.Count; OrtMs = (Mean $ort); OrtMedianMs = (Median $ort); ModelMs = (Mean $model); ChunkMs = (Mean $chunk)
        Ep = (@($eps.Keys | Sort-Object) -join '+'); DirectML = $dml
    }
}

function Get-DeckSeconds([string]$text) {
    $text = $text.Replace([char]'o', [char]'0').Replace([char]'O', [char]'0')   # OCR reads 00:00.0 as oo:oo.o (the PC, 2026-11-19)
    foreach ($m in [regex]::Matches($text, '(-?)\s?(\d{1,2})[:.](\d{2})[.,](\d)')) {
        if (-not $m.Groups[1].Value) { return [int]$m.Groups[2].Value * 60 + [int]$m.Groups[3].Value + [int]$m.Groups[4].Value / 10 }
    }
    return $null
}

# SAMPLES: @(wall, deck) pairs in time order (deck $null: not read). Each step between two read
# samples: the deck's advance against the wall clock's; a step under half of it (or backwards) is a
# stall. Read: the samples read; Stalls: the stalled steps; Worst: the lowest ratio
function Get-Advance($samples) {
    $read = @($samples | Where-Object { $null -ne $_[1] })
    $stalls = 0; $worst = $null
    for ($i = 1; $i -lt $read.Count; $i++) {
        $dw = $read[$i][0] - $read[$i - 1][0]; $dd = $read[$i][1] - $read[$i - 1][1]
        if ($dw -le 0) { continue }
        $ratio = [math]::Round($dd / $dw, 2)
        if ($null -eq $worst -or $ratio -lt $worst) { $worst = $ratio }
        if ($ratio -lt 0.5) { $stalls++ }
    }
    [pscustomobject]@{ Samples = @($samples).Count; Read = $read.Count; Stalls = $stalls; Worst = $worst }
}
