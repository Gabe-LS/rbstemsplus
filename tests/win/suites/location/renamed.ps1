# As the user: the label renamed while plugged in: the measure names it, and the hint follows (NL1)
$st = Read-Sticks
foreach ($a in Get-LocArchs) {
    "INFO stick D labelled DLAB since: volname $(Loc $a 'volname' 'volname' @("$($st.D):\")) ($a)"
    $sup = Get-LocSupport $a
    $f = Get-Content -Raw -LiteralPath (Join-Path $sup '..\facts.json') | ConvertFrom-Json
    Expect (Loc $a 'measure' 'measure' @($sup, (Get-LocCache $a))) "^0 1 0 -?\d+\|RBRENAMED\|$(Rx $f.gdir)$" "the measure: the new label ($a)"
    Check ((Get-Hint $sup 'location-drive') -eq "dir=$($f.gdir)`nlabel=RBRENAMED`n") "location-drive follows it ($a): $(Get-Hint $sup 'location-drive')"
}
Done
