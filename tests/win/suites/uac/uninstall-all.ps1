# The complete uninstall with Stems Cache (G4; app.md 5.9, release.md section 8), as the user:
# Uninstall RB Stems Plus Completely > Uninstall, its UAC prompt answered Yes by the arm step's
# rbui, then the uninstall's own dialogs (unlib's Complete-Uninstall). By this step's name:
#   un1*     UN1: UAC first (the apps folder still there while the prompt shows), then
#            everything gone (unlib's Test-UninstallGone), rekordbox's onnxruntime.dll Pioneer's
#            again, no Stems Cache root
#   held*    UN9's reboot branch (J7): with the bin\ worker held open (hold step), the app's side
#            goes; the root stays for the restart (checked as SYSTEM by the j7-* steps)
#   final*   UN9's last uninstall after the restart: the prompt's details (arm-details) name the
#            bin\ worker in Program Files, never the apps folder (J7: nothing elevated from the
#            app folder); the app's side goes
$l = Get-Layout
$f = Read-WorkerFacts
if (-not $f) { Skip 'no facts from the prep step (it failed: its log)' }
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
if (@(Get-Rekordbox).Count) { Check (Close-Rekordbox 200) 'rekordbox closed first' }
$case = ($VmRun.Step -split '-')[0]
Open-App
if (-not $script:app) { Done }
$app = $script:app
Invoke-UninstallAll
if (-not $script:Pressed) { Done }
$shown = Wait-Until { Test-ConsentShown } 120
Check $shown "UAC's prompt shows (consent.exe)"
Check (Test-Path -LiteralPath $l.Apps) 'UAC first: the apps folder is still there while the prompt shows'
# the uninstall's end (its Done; the welcome can't come back: a quiet start)
$done = Invoke-UiNamed $app.Id (Get-CoreText 'T_DONE') 300 'Button'
Check $done 'its end: Done'
if (-not $done) {
    Show-AppState   # what shows instead, and app.log's tail
    # what Stems Cache's root holds as this user sees it, and the boot-time deletes naming it
    # (host_facts' root_pending_only reads both)
    if (Test-Path -LiteralPath $CacheRoot) {
        try {
            foreach ($i in @(Get-ChildItem -LiteralPath $CacheRoot -Recurse -Force -ErrorAction Stop)) { "root: $($i.FullName) [$($i.Attributes)]" }
        } catch { "root: not listed whole ($($_.Exception.Message))" }
    }
    $sm = Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager' -ErrorAction SilentlyContinue
    foreach ($v in 'PendingFileRenameOperations', 'PendingFileRenameOperations2') {
        $vals = @($sm.$v)
        "${v}: $($vals.Count) strings"
        for ($i = 0; $i -lt $vals.Count; $i++) { if ($vals[$i] -match 'RB Stems Plus') { "  [$i] '$($vals[$i])' then '$(if ($i + 1 -lt $vals.Count) { $vals[$i + 1] })'" } }
    }
}
Check (Wait-Until { $app.HasExited } 90) 'the app quits'
Test-UninstallGone
Test-ConsentResult yes
switch ($case) {
    'un1' {
        Check ((Get-Sha $RbOrt) -eq $f.PioneerSha) "rekordbox's onnxruntime.dll is Pioneer's again"
        # the bin\ worker can't delete its own running image: its reboot branch (I20, J7) leaves the
        # root holding only boot-time deletes, gone at the next restart (the final case's restart
        # checks that); anything else left in it is a failure
        $gone = Wait-Until { -not (Test-Path -LiteralPath $CacheRoot) } 60
        $pending = (-not $gone) -and (Test-PendingOnly $CacheRoot)
        Check ($gone -or $pending) "Stems Cache's root is gone, or holds only boot-time deletes ($CacheRoot$(if ($pending) { ': removed at the next restart' }))"
    }
    'held' { Check ((Get-Sha $RbOrt) -eq $f.PioneerSha) "rekordbox's onnxruntime.dll is Pioneer's again" }
    'final' {
        $prompt = $script:ConsentResult.Prompt
        Check ($prompt -match [regex]::Escape("$CacheRoot\bin\")) "J7: the prompt's program is the bin\ worker in $CacheRoot ($prompt)"
        Check ($prompt -notmatch [regex]::Escape($l.Apps)) 'and nothing from the apps folder'
        Check ((Get-Sha $RbOrt) -eq $f.PioneerSha) "rekordbox's onnxruntime.dll is Pioneer's again"
    }
    default { Check $false "no case for a step named $($VmRun.Step)" }
}
Done
