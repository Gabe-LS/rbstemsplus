# sac/lib.ps1: sac.suite's and bootstrap-sac.suite's own helpers (their include, in place of
# lib/vmlib.ps1). Smart App Control On may run an unsigned script in ConstrainedLanguage (the VM
# round of 2026-11-17 found ours, without Mark of the Web, in FullLanguage; each step says its
# mode), where vmlib's .NET calls would stop a step before it begins: so cmdlets, core types and
# native programs only here (no Add-Type, no [Console], no non-core type's methods).
$ErrorActionPreference = 'Stop'
$script:SacChecks = 0
$script:SacFails = 0
function Check([bool]$ok, [string]$what) {
    $script:SacChecks++
    if ($ok) { "PASS $what" } else { $script:SacFails++; "FAIL $what" }
}
function Skip([string]$why) { "SKIP $why"; exit 77 }
function Done {
    "$($VmRun.Step): $script:SacChecks checks, $script:SacFails failed"
    if ($script:SacFails -gt 0 -or $script:SacChecks -eq 0) { exit 1 }
    exit 0
}
"language mode: $($ExecutionContext.SessionState.LanguageMode)"
$SacKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy'
function Get-SacState { return (Get-ItemProperty -LiteralPath $SacKey -ErrorAction SilentlyContinue).VerifiedAndReputablePolicyState }
function Get-MpSac { try { return [string](Get-MpComputerStatus -ErrorAction Stop).SmartAppControlState } catch { return "unread: $($_.Exception.Message)" } }

# whoami works in ConstrainedLanguage. Require the interactive user's High token (SYSTEM has
# a different mandatory level).
function Test-SacHighToken {
    return [bool](@(whoami.exe /groups | Where-Object { $_ -match '\bS-1-16-12288\b' }).Count)
}
