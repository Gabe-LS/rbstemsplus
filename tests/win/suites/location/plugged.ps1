# As the user, both sticks plugged in: their names; the location choice on stick A (LOCATION_PROBE's
# refusal of a VHDX, then CREATE and WRITE through the folder's GUID path, as the probe gives it),
# the measure (AVAILABLE, the label remembered once), DISCOVER (itself, and through the hint from
# another folder), a 300+ character path's name, the unlabelled stick's name and its no-hint; a
# second location on the unlabelled stick B (support-b), measured AVAILABLE by its letter
$st = Read-Sticks
$SA = "$($st.A):"; $SB = "$($st.B):"
$free = Get-FreeLetter @('Q', 'Y', 'X', 'K', 'J')
foreach ($a in Get-LocArchs) {
    $sup = Get-LocSupport $a
    Get-ChildItem -LiteralPath $sup -Force | Remove-Item -Recurse -Force
    # the names (NL1, NL4, NL5, NR1)
    Expect (Loc $a 'volname' 'volname' @("$SA\")) "^0 RBSTICK\|$SA$" "volname of $SA\ ($a)"
    Expect (Loc $a 'volname' 'volname' @($st.GA)) "^0 RBSTICK\|$SA$" "volname of its GUID path ($a)"
    Expect (Loc $a 'volname' 'volname' @("$SB\")) "^0 \|$SB$" "volname of the unlabelled $SB\ ($a)"
    Expect (Loc $a 'volname' 'volname' @("$($st.D):\")) "^0 \|$($st.D):$" "volname of the unlabelled NTFS $($st.D):\ ($a)"
    Expect (Loc $a 'drive' 'drive' @($sup, "$($st.D):\x")) "^\[$($st.D):\]$" "the unlabelled NTFS stick's name ($a)"
    $r = Loc $a 'volname' 'volname' @('\\?\Volume{00000000-0000-0000-0000-000000000000}\x')
    Expect $r '^[1-9]' "a zero GUID: no volume ($a)"; "INFO raw GetVolumePathNameW: $(Last-Line 'raw')"
    if ($free) {
        $r = Loc $a 'volname' 'volname' @("${free}:\x")
        Expect $r '^[1-9]' "an unassigned ${free}: no volume ($a)"; "INFO raw GetVolumePathNameW: $(Last-Line 'raw')"
    }
    Expect (Loc $a 'drive' 'drive' @($sup, "$SB\x")) "^\[$SB\]$" "the unlabelled stick's name ($a)"
    # LOCATION_PROBE: a VHDX is a file-backed virtual disk: internal (1); the user's own folder too
    $music = "$SA\Music-$a"
    New-Item -ItemType Directory -Force $music | Out-Null
    Expect (Loc $a 'choice-probe' 'choice' @($music)) '^0 1\|\|\|RBSTICK$' "LOCATION_PROBE of a VHDX folder: internal ($a)"
    "INFO facts: $(Last-Line 'facts')"
    Expect (Loc $a 'choice-probe' 'choice' @($env:USERPROFILE)) '^0 1\|' "LOCATION_PROBE of the profile: internal ($a)"
    # CREATE and WRITE through the GUID path
    $c = Loc $a 'create' 'create' @($sup, $music)
    $gdir = "$($st.GA)Music-$a\$CacheName"
    Expect $c "^0 $(Rx $gdir)\|[0-9A-F-]{36}\|\|indexed 1$" "CREATE: our folder by its GUID path, the not-indexed attribute ($a)"
    $id = ($c -split '\|')[1]
    Expect (Last-Line 'write') '^0$' "WRITE ($a)"
    Expect (Last-Line 'config') "^$(Rx $gdir)\|$(Rx $id)$" "config.ini's cache_dir: the volume-GUID form ($a)"
    Check (Test-Path -LiteralPath "$music\$CacheName\.rbstemsplus-cache") "the marker is there ($a)"
    Check ((Get-Hint $sup 'location-hint') -eq "mount=$($st.GA)`nrelative=Music-$a\$CacheName`nown=$id`n") "location-hint ($a): $(Get-Hint $sup 'location-hint')"
    Check ((Get-Hint $sup 'location-drive') -eq "dir=$gdir`nlabel=RBSTICK`n") "location-drive written at the choice ($a): $(Get-Hint $sup 'location-drive')"
    # the measure: chosen, AVAILABLE, the label; the hint unchanged
    $before = (Get-Item -LiteralPath (Join-Path $sup 'location-drive')).LastWriteTimeUtc
    Expect (Loc $a 'measure' 'measure' @($sup, (Get-LocCache $a))) "^0 1 0 -?\d+\|RBSTICK\|$(Rx $gdir)$" "the measure: AVAILABLE ($a)"
    Check ((Get-Item -LiteralPath (Join-Path $sup 'location-drive')).LastWriteTimeUtc -eq $before) "location-drive written once ($a)"
    Expect (Loc $a 'probe' 'probe' @("$music\$CacheName", $id)) '^1$' "the saved probe by the letter path: NOT_CONNECTED ($a)"
    Expect (Loc $a 'probe' 'probe' @($gdir, $id.ToLowerInvariant())) '^0$' "the probe by the GUID path, the id in lower case ($a)"
    # DISCOVER: the folder itself, then from another folder through the hint
    Expect (Loc $a 'discover' 'discover' @($sup, $music)) "^0 2\|$(Rx $gdir)\|$(Rx $id)\|$(Rx $gdir)$" "DISCOVER finds it, the current one ($a)"
    New-Item -ItemType Directory -Force "$SA\Elsewhere-$a" | Out-Null
    Expect (Loc $a 'discover' 'discover' @($sup, "$SA\Elsewhere-$a")) "^0 2\|$(Rx $gdir)\|" "DISCOVER through the hint ($a)"
    # a path of 300+ characters on it: the label (NL5)
    $long = "$SA\Music-$a\" + (@(1..12 | ForEach-Object { 'a-long-folder-name-for-nl5' }) -join '\')
    [void][IO.Directory]::CreateDirectory("\\?\$long")
    Check ($long.Length -gt 300) "a $($long.Length)-character path"
    Expect (Loc $a 'drive' 'drive' @($sup, $long)) '^\[RBSTICK\]$' "a long path's name ($a)"
    # the unlabelled stick, remembered: nothing written over A's label
    Expect (Loc $a 'drive' 'drive' @($sup, "$SB\x", '1')) "^\[$SB\]$" "the unlabelled stick remembered ($a)"
    Check ((Get-Hint $sup 'location-drive') -eq "dir=$gdir`nlabel=RBSTICK`n") "location-drive kept ($a)"
    # the unlabelled stick B with a location of its own (support-b): measured AVAILABLE, named by its
    # letter; unplugged.ps1 measures it again once B is gone (review N3R5)
    $supb = Get-LocSupportB $a
    Get-ChildItem -LiteralPath $supb -Force | Remove-Item -Recurse -Force
    $musicb = "$SB\Music-$a"
    New-Item -ItemType Directory -Force $musicb | Out-Null
    $cb = Loc $a 'create' 'create' @($supb, $musicb)
    $gdirb = "$($st.GB)Music-$a\$CacheName"
    Expect $cb "^0 $(Rx $gdirb)\|[0-9A-F-]{36}\|\|indexed [01]$" "CREATE on the unlabelled stick B, by its GUID path ($a)"
    $idb = ($cb -split '\|')[1]
    Expect (Last-Line 'write') '^0$' "WRITE on stick B ($a)"
    Expect (Loc $a 'measure' 'measure' @($supb, (Join-Path $supb 'Cache'))) "^0 1 0 -?\d+\|$SB\|$(Rx $gdirb)$" "stick B's location measured: AVAILABLE, named by its letter ($a)"
    "INFO stick B's location-drive: [$(Get-Hint $supb 'location-drive')]"
    ConvertTo-Json @{ gdir = $gdir; id = $id; gdirb = $gdirb; idb = $idb } | Set-Content -LiteralPath (Join-Path $sup '..\facts.json') -Encoding UTF8
}
Done
