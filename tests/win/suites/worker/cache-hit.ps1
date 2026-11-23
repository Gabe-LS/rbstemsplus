# The cache hit (release.md section 8: one on each newly listed rekordbox version; bridge.md 4's
# end to end: a track loaded twice in rekordbox, the second load a hit in bridge.log). No step can
# load a track in rekordbox yet (nothing the suites drive does it), so this step SKIPs, which
# --record writes as .failed: this suite's record isn't a pass and the worker row fails closed.
# The hit is by hand until a step can do it (by-hand.md).
# OPEN OWNER QUESTION, not decided here: can a recorded by-hand W13a round stand in for this
# suite's record? Nothing here or in vm-run.sh check treats one as a pass.
# TODO (review WS), all in the commit that makes this step real, never before it:
#   WS2: cache-files.ps1's ACL oracle in full: the root owned by Administrators with a protected
#        DACL of exactly SYSTEM 0x1F01FF, Administrators 0x1F01FF, Users 0x1200A9 (none
#        inherited, each CI|OI); bin, bin\<sha12>, the worker exe, ort, ort\onnxruntime.dll and
#        pioneer with its contents, and Program Files itself: owner Administrators, SYSTEM or
#        TrustedInstaller, no reparse point, no other SID's Allow ACE with a write, delete, ACL,
#        owner, GENERIC_WRITE or GENERIC_ALL right; its header saying what it checks;
#   WS5: the rekordbox version from the snapshot's name ("rekordbox V installed"), prep checking
#        ProductVersion against it, and a rule (check, or the selftest over release.list) that
#        every rekordbox version DIR/payload.json's windows.compatible.rekordbox newly lists has
#        its worker row;
# Remove worker:cache-hit from first-run.sh's GAPS when the release gate accepts the PC record (M6).
$log = Join-Path (Get-Layout).Support 'Logs\bridge.log'
$entry = (Read-BridgeLog $log 0) -match 'bridge loaded, ENTRY ONLY'
Skip "no step loads a track in rekordbox yet, so no cache hit is checked$(if ($entry) { ' (and this bridge is entry-only: it caches nothing)' }): by hand (by-hand.md), and this suite's record is never a pass"
