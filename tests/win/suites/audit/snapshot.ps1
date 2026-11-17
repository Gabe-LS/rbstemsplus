# The trace audit's snapshot (release.md section 8): the step's name after "audit-" names it
# (audit-before, audit-installed, ...), kept in C:\rbtest\audit\ for audit/after-uninstall.ps1
$name = $VmRun.Step -replace '^audit-', ''
Save-Snapshot $name
Check (Test-Path -LiteralPath (Join-Path $AuditDir "$name.json")) "snapshot $name saved"
Done
