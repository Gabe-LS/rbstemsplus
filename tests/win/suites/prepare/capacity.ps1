# N1.9: use real playlists with estimates on either side of the free-space boundary.
# C:\rbtest\prepare-capacity.json names the actual cache directory and the two fixture
# estimates (GiB, from the real helper for each playlist at the fixture's current limit).
# [ { "Playlist":"RBSP fits with a raise", "Cache":"C:\\...\\Cache",
#     "CurrentGiB":20, "RequiredGiB":21 },
#   { "Playlist":"RBSP cannot raise", "Cache":"C:\\...\\Cache",
#     "CurrentGiB":20, "RequiredGiB":1000 } ]
$fixture = 'C:\rbtest\prepare-capacity.json'
if (-not (Test-Path -LiteralPath $fixture)) { Skip "needs the two non-fitting playlists and their real estimates in $fixture" }
$cases = @(Get-Content -LiteralPath $fixture -Raw | ConvertFrom-Json)
Expect ($cases.Count -eq 2 -and $cases[0].Playlist -ne $cases[1].Playlist) 'two different capacity fixture playlists were supplied'
$results = @()
try {
    Start-PrepareApp
    $appPid = $script:ShellApp.Id
    foreach ($entry in $cases) {
        $name = [string]$entry.Playlist
        $cache = [string]$entry.Cache
        Expect ($cache -match '^[A-Za-z]:\\' -and (Test-Path -LiteralPath $cache -PathType Container)) 'the local cache fixture directory exists'
        Expect ($entry.RequiredGiB -gt $entry.CurrentGiB -and $entry.CurrentGiB -gt 0) 'the helper estimate requires a higher limit'
        $volume = New-Object IO.DriveInfo (Split-Path -Qualifier $cache)
        $free = $volume.AvailableFreeSpace
        $raise = Get-PrepareCapacity $entry.CurrentGiB $entry.RequiredGiB $free
        $results += [bool]$raise
        "capacity: $name, $($volume.Name) has $free available bytes"
        Use-ShellUi invoke --pid $appPid $name --type TreeItem --timeout 30
        Use-ShellUi invoke --pid $appPid (Get-CoreText 'T_BTN_PREPARE') --type Button --timeout 30
        $title = (Get-CoreText 'T_PREP_DOESNT_FIT').Replace('{playlist}', $name)
        Use-ShellUi wait-window --pid $appPid --title $title --timeout 20
        Use-ShellUi read --pid $appPid --title $title --timeout 10
        $options = $script:ExeOut
        $limitPattern = [regex]::Escape((Get-CoreText 'T_FIT_RAISE')).Replace('\{limit}', '[0-9.]+')
        if ($raise) {
            $label = (Get-CoreText 'T_FIT_RAISE').Replace('{limit}', $raise)
            Check (Test-A11yNode $options 'Button' $label) "the offered limit matches the independent free-space check ($label)"
        } else {
            Check (($options -notmatch "Button `"$limitPattern`"") -and $options.Contains((Get-CoreText 'T_FIT_NO_ROOM').Trim())) 'free space rules out a higher limit'
        }
        Use-ShellUi invoke --pid $appPid --title $title (Get-CoreText 'T_CANCEL') --type Button --timeout 10
    }
    Check (($results -contains $true) -and ($results -contains $false)) 'the fixtures exercised both sides of the available-space boundary'
} finally { Stop-App }
Done
