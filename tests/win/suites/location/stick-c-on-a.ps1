# As the user, stick C on A's letter: the GUID config is still NOT_CONNECTED; the letter-form config
# (unplugged.ps1's support-letter) is also NOT_CONNECTED, with C's live label and A's hint kept
# (NR4). Probing C's GUID path reaches the folder and finds its marker missing.
$st = Read-Sticks
$SA = "$($st.A):"
foreach ($a in Get-LocArchs) {
    $sup = Get-LocSupport $a
    Expect (Loc $a 'measure' 'measure' @($sup, (Get-LocCache $a))) '^0 1 1 -?\d+\|RBRENAMED\|-$' "another stick on A's letter: the GUID config still NOT_CONNECTED ($a)"
    $sl = Join-Path $env:TEMP "rbsp-n3\$a\support-letter"
    $ldir = "$SA\Music-$a\$CacheName"
    Check ((Get-Hint $sl 'location-drive') -eq "dir=$ldir`nlabel=RBSTICK`n") "the letter config from unplugged.ps1 ($a)"
    Expect (Loc $a 'measure' 'measure' @($sl, (Join-Path $sl 'Cache'))) '^0 1 1 -?\d+\|OTHER\|-$' "a letter config on another stick: NOT_CONNECTED ($a)"
    Check ((Get-Hint $sl 'location-drive') -eq "dir=$ldir`nlabel=RBSTICK`n") "the hint keeps A's label ($a): $(Get-Hint $sl 'location-drive')"
    $facts = Get-Content -Raw -LiteralPath (Join-Path $sup '..\facts.json') | ConvertFrom-Json
    Expect (Loc $a 'probe' 'probe' @("$($st.GC)Music-$a\$CacheName", $facts.id)) '^3$' "stick C by its GUID: MARKER_MISSING_OR_WRONG ($a)"
}
Done
