# F2 and L1 on stick A before replugged.ps1 deletes the cache.
$st = Read-Sticks
foreach ($a in Get-LocArchs) {
    $sup = Get-LocSupport $a
    $cache = "$($st.A2):\Music-$a\$CacheName"
    Expect (Loc $a 'remove-held' 'remove-held' @($sup, $cache)) '^4 1$' "whole-cache removal refuses a competing DELETE handle and blocks rename while held ($a)"
    $child = Join-Path $cache 'ancestor\in-use'
    New-Item -ItemType Directory -Force -Path $child -ErrorAction Stop | Out-Null
    Set-Content -LiteralPath (Join-Path $child 'stems') -Value 'keep' -Encoding ASCII -ErrorAction Stop
    Expect (Loc $a 'remove-child-held' 'remove-child-held' @($sup, $child)) '^7 1$' "cache walk refuses the competing DELETE handle on a nested directory and preserves its contents ($a)"
}
Done
