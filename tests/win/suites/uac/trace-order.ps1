# Compare full UTC ticks from the TEST shell's handlers and the existing rbui responder.
function Get-SessionTraceErrors([string]$case, [string]$app, [string]$screen) {
    $times = @{}
    foreach ($line in ($app -split "`r?`n")) {
        if ($line -match '^(\d+) (query-veto|query-allow|end-true|end-false)$') { $times[$Matches[2]] += @([long]$Matches[1]) }
    }
    foreach ($line in ($screen -split "`r?`n")) {
        if ($line -match '^\d\d:\d\d:\d\d\.\d{3} session-trace (\d+) (block|answer)$') { $times[$Matches[2]] += @([long]$Matches[1]) }
    }
    if ($times['query-veto'].Count -ne 1 -or $times['query-allow'].Count -ne 0 -or $times['block'].Count -ne 1) {
        'expected one vetoed query and one block-screen observation'
        return
    }
    $block = $times['block'][0]
    if ($times['query-veto'][0] -gt $block) { 'block screen was observed before the vetoed query' }
    if ($case -eq 'wc1') {
        # The veto path can close the app before Windows ends the session.
        if ($times['answer'].Count -or $times['end-false'].Count -or $times['end-true'].Count -gt 1) {
            'automatic sign-out had an answer, a cancellation, or repeated end messages'
        }
        if ($times['end-true'].Count -and $times['end-true'][0] -lt $block) { 'the session ended before the block screen was seen' }
        return
    }
    $want, $unwanted = if ($case -eq 'wc2') { 'end-false', 'end-true' } else { 'end-true', 'end-false' }
    if ($times['answer'].Count -ne 1 -or $times[$want].Count -ne 1 -or $times[$unwanted].Count -ne 0) {
        "expected one block-screen answer, one $want, and no $unwanted"
        return
    }
    $answer = $times['answer'][0]
    if ($answer -lt $block -or $times[$want][0] -lt $answer) { "expected block screen, answer, then $want" }
}
