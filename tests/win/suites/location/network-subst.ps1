# As the user: a mapped network drive (the share \\127.0.0.1\rbn3): LOCATION_PROBE refuses it as a
# network drive (2), a location there probes NETWORK (5); a SUBST drive as the default cache: the
# 50 GB floor while the free bytes are under it (total unknown, review NL6)
$net = Get-FreeLetter @('N', 'Y', 'X', 'K')
$sub = Get-FreeLetter @('Q', 'J', 'I', 'H')
& net.exe use "${net}:" '\\127.0.0.1\rbn3' /persistent:no 2>&1 | Out-String | ForEach-Object { "INFO net use: $_" }
Check (Test-Path "${net}:\") "${net}: mapped"
$id = '11111111-2222-4333-8444-555555555555'
$archs = Get-LocArchs
foreach ($a in $archs) {
    New-Item -ItemType Directory -Force "${net}:\m-$a\$CacheName" | Out-Null
    Expect (Loc $a 'choice-probe' 'choice' @("${net}:\m-$a")) '^0 2\|' "LOCATION_PROBE of a mapped drive: network ($a)"
    Expect (Loc $a 'probe' 'probe' @("${net}:\m-$a\$CacheName", $id)) '^5$' "the probe of a location there: NETWORK ($a)"
}
& net.exe use "${net}:" /delete /y 2>&1 | Out-Null
# SUBST: the default cache on a drive with no volume information
$root = Join-Path $env:TEMP 'rbsp-n3\substroot'
New-Item -ItemType Directory -Force $root | Out-Null
& subst.exe "${sub}:" $root
Check (Test-Path "${sub}:\") "${sub}: substituted"
$freeC = (Get-PSDrive -Name C).Free
foreach ($a in $archs) {
    # the This PC cache is always SUPPORT\Cache (open_pc_support, 65df1f75): the support folder
    # itself on the SUBST drive, its Cache not made yet (the probe: AVAILABLE)
    $sup = "${sub}:\support-subst-$a"
    New-Item -ItemType Directory -Force $sup | Out-Null
    # the bridge's measure (plat_free_bytes): 0 when it can't be read, on which it pauses (app.md §5.8)
    # the measure reads the in-use folder's volume (volume_path: the nearest existing folder), here
    # the support folder; the drive's root is read too, for the record (it read 0 on 2026-11-21)
    "INFO free bytes on ${sub}:\ ($a): $(Loc $a 'free' 'free' @("${sub}:\"))"
    $fr = Loc $a 'free' 'free' @($sup)
    "INFO free bytes on $sup ($a): $fr"
    $avail = [double](($fr -split ' ')[0])
    $want = if ($avail -lt 50e9) { '50000000000' } else { '-1' }
    if ($avail -lt 50e9 -and $freeC -ge 50e9) { "INFO FINDING: plat_free_bytes reads $avail bytes in $sup on the SUBST drive while C: has $freeC free: the bridge would pause there" }
    Expect (Loc $a 'measure' 'measure' @($sup, "$sup\Cache")) "^0 0 0 $want\|-\|$(Rx "$sup\Cache")$" "This PC on a SUBST drive: the floor $want (C: has $freeC bytes free; $a)"
}
& subst.exe "${sub}:" /d
Done
