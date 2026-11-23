# As SYSTEM, for the "rekordbox updating" case (rb-moved, rb-back): rekordbox 7.2.19's version
# folder moved aside (to C:\Program Files\rekordbox\rbtest-moved, a name no version folder has),
# so Uninstall Stems Cache takes its second prompt's path ("rekordbox missing": remove the copy of
# rekordbox's files, the remove-root-folder run, AF8's case); then moved back. rekordbox is never
# started; a fake updater left running by the user step is stopped here (rb-back).
$moved = Join-Path (Split-Path $RbDir) 'rbtest-moved'
if ($VmRun.Step -eq 'rb-moved') {
    if (@(Get-Rekordbox).Count) { Skip 'rekordbox runs (the snapshot has it open): its folder stays' }
    Check (Test-Path -LiteralPath $RbExe) "rekordbox 7.2.19's folder is there ($RbDir)"
    Rename-Item -LiteralPath $RbDir -NewName (Split-Path -Leaf $moved)
    Check ((Test-Path -LiteralPath $moved) -and -not (Test-Path -LiteralPath $RbDir)) "moved aside to $moved"
} else {
    # only the fake updater updating-cancel started (in the user's %TEMP%\rbtest-upd), never another
    Get-CimInstance Win32_Process -Filter "Name='Upmgr rekordbox.exe'" -ErrorAction SilentlyContinue |
        Where-Object { [string]$_.ExecutablePath -like '*\rbtest-upd\Upmgr rekordbox.exe' } |
        ForEach-Object { "stopping the fake updater: $($_.ExecutablePath)"; Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $moved) { Rename-Item -LiteralPath $moved -NewName (Split-Path -Leaf $RbDir) }
    Check ((Test-Path -LiteralPath $RbExe) -and -not (Test-Path -LiteralPath $moved)) "rekordbox 7.2.19's folder is back ($RbDir)"
}
Done
