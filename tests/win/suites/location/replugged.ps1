# As the user, stick A back on another letter: the GUID config is AVAILABLE again (letters change
# between plugs: the contract with the bridge); DISCOVER through the new letter finds the current
# folder; REMOVE_LOCATION by the GUID path deletes it whole; Use This PC takes cache_dir out
$st = Read-Sticks
$A2 = "$($st.A2):"
foreach ($a in Get-LocArchs) {
    $sup = Get-LocSupport $a
    $f = Get-Content -Raw -LiteralPath (Join-Path $sup '..\facts.json') | ConvertFrom-Json
    Expect (Loc $a 'measure' 'measure' @($sup, (Get-LocCache $a))) "^0 1 0 -?\d+\|RBRENAMED\|$(Rx $f.gdir)$" "the measure on the new letter: AVAILABLE ($a)"
    "INFO the user sees $A2\: $(Test-Path "$A2\") ($a)"
    Expect (Loc $a 'discover' 'discover' @($sup, "$A2\Music-$a")) "^0 2\|$(Rx $f.gdir)\|$(Rx $f.id)\|$(Rx $f.gdir)$" "DISCOVER by the new letter: the current folder ($a)"
    Expect (Loc $a 'remove' 'remove' @($sup)) '^0 \|$' "REMOVE_LOCATION (whole) by the GUID path ($a)"
    Check (-not (Test-Path -LiteralPath "$A2\Music-$a\$CacheName")) "our folder is gone ($a)"
    Check (Test-Path -LiteralPath "$A2\Music-$a") "the folder above it stays ($a)"
    Expect (Loc $a 'this-pc' 'this-pc' @($sup)) '^0$' "Use This PC ($a)"
    $cfg = [string](Get-Content -Raw -LiteralPath (Join-Path $sup 'config.ini'))
    Check ($cfg -notmatch 'cache_dir' -and $cfg -notmatch 'cache_id') "config.ini without the location ($a): $cfg"
}
Done
