# As the user, stick A mounted in a folder with no letter: its name is its label with letter "";
# probe through the mount folder from the system volume's GUID root: LINK (the bridge's "folder");
# LOCATION_PROBE of a folder through it: the volume's refusal first (a VHDX: internal), INFO
$st = Read-Sticks
$mf = 'C:\mnt\n3a'
foreach ($a in Get-LocArchs) {
    Expect (Loc $a 'volname' 'volname' @("$mf\")) '^0 RBRENAMED\|$' "volname through the mount folder: the label, no letter ($a)"
    $id = '11111111-2222-4333-8444-555555555555'
    New-Item -ItemType Directory -Force "$mf\Music-$a\$CacheName" | Out-Null
    Expect (Loc $a 'probe' 'probe' @("$($st.SystemGuid)mnt\n3a\Music-$a\$CacheName", $id)) '^4$' "the saved GUID path through the mount folder: LINK ($a)"
    "INFO LOCATION_PROBE through it ($a): $(Loc $a 'choice-probe' 'choice' @("$mf\Music-$a"))"
}
Done
