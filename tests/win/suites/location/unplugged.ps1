# As the user, A and B unplugged and A's letter free: the GUID config is NOT_CONNECTED, named by the
# hinted label, the hint unchanged; the unlabelled stick's name is "" (never its letter), and its
# own location (support-b) measures NOT_CONNECTED with "" as its drive (review N3R5); a config
# naming A by its letter (hand-written, support-letter) measures NOT_CONNECTED by the hinted label
# while the letter is free (N3R5), the hint kept
$st = Read-Sticks
$SA = "$($st.A):"; $SB = "$($st.B):"
Check (-not (Test-Path -LiteralPath "$SA\")) "$SA is free in the user's session"
foreach ($a in Get-LocArchs) {
    $sup = Get-LocSupport $a
    $f = Get-Content -Raw -LiteralPath (Join-Path $sup '..\facts.json') | ConvertFrom-Json
    $hint = Get-Hint $sup 'location-drive'
    Expect (Loc $a 'measure' 'measure' @($sup, (Get-LocCache $a))) '^0 1 1 -?\d+\|RBRENAMED\|-$' "the measure: NOT_CONNECTED, the hinted label ($a)"
    Check ((Get-Hint $sup 'location-drive') -eq $hint) "location-drive unchanged ($a)"
    Expect (Loc $a 'volname' 'volname' @($st.GA)) '^[1-9]' "volname of A's GUID path: not there ($a)"
    Expect (Loc $a 'drive' 'drive' @($sup, "$SB\x")) '^\[\]$' "the unlabelled stick unplugged: no name ($a)"
    # the unlabelled stick's own location, unplugged: NOT_CONNECTED, "" (the nameless form)
    $supb = Get-LocSupportB $a
    $hintb = Get-Hint $supb 'location-drive'
    Expect (Loc $a 'measure' 'measure' @($supb, (Join-Path $supb 'Cache'))) '^0 1 1 -?\d+\|\|-$' "stick B's location unplugged: NOT_CONNECTED, drive name `"`" ($a)"
    Check ((Get-Hint $supb 'location-drive') -eq $hintb) "stick B's location-drive unchanged ($a): [$hintb]"
    # the letter form (a hand-written config) while its letter is free
    $sl = Join-Path $env:TEMP "rbsp-n3\$a\support-letter"
    New-Item -ItemType Directory -Force $sl | Out-Null
    Get-ChildItem -LiteralPath $sl -Force | Remove-Item -Recurse -Force
    $ldir = "$SA\Music-$a\$CacheName"
    Set-Content -LiteralPath (Join-Path $sl 'config.ini') -Value "cache_dir = `"$ldir`"`ncache_id = $($f.id)" -Encoding ASCII -NoNewline
    [IO.File]::WriteAllText((Join-Path $sl 'location-drive'), "dir=$ldir`nlabel=RBSTICK`n")
    Expect (Loc $a 'measure' 'measure' @($sl, (Join-Path $sl 'Cache'))) '^0 1 1 -?\d+\|RBSTICK\|-$' "a letter config, its letter free: NOT_CONNECTED, the hinted label ($a)"
    Check ((Get-Hint $sl 'location-drive') -eq "dir=$ldir`nlabel=RBSTICK`n") "the hint kept ($a): $(Get-Hint $sl 'location-drive')"
}
Done
