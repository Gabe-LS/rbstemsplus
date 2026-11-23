# UN9's reboot branch (J7) and the root after the last restart (R4.I77), as SYSTEM (the root's
# DACL lets the user read only part of it), by this step's name:
#   j7-pending    after the held uninstall: the worker's image renamed to a *.del in
#                 bin\<sha12>\ and that name in PendingFileRenameOperations (the restart deletes
#                 it); then the hold released
#   j7-restarted  after the reinstall, Install Stems Cache and a restart: the bin\ worker present
#                 (the payload's), no *.del left
#   root-gone     after the last uninstall and a restart: no Stems Cache root
$p = Read-TestPayload
$admin = ([string]$p.windows.app.admin_sha256).ToLowerInvariant()
$dir = Join-Path $CacheRoot "bin\$($admin.Substring(0, 12))"
$dels = @(Get-ChildItem -LiteralPath $CacheRoot -Recurse -Force -File -Filter '*.del' -ErrorAction SilentlyContinue | ForEach-Object FullName)
switch -Regex ($VmRun.Step) {
    '^j7-pending' {
        $mine = @($dels | Where-Object { $_ -like "$dir\RB Stems Plus.exe.*.del" })
        if ($mine.Count -eq 0) { Missing "J7: no *.del in ${dir}: the uninstall didn't take the reboot branch (the hold didn't stop the POSIX delete)" }
        else {
            Check $true "J7: the worker's image renamed for the restart ($($mine -join ', '))"
            $pend = @((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager' -Name PendingFileRenameOperations -ErrorAction SilentlyContinue).PendingFileRenameOperations)
            Check (@($pend | Where-Object { $_ -and $_.EndsWith($mine[0], [StringComparison]::OrdinalIgnoreCase) }).Count -ge 1) 'and named in PendingFileRenameOperations'
        }
        # the queue's raw strings naming the app, each with the string after it (a delete's
        # target: ''), as Windows wrote them (host_facts' hf_pending_parse reads these forms)
        $sm = Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager' -ErrorAction SilentlyContinue
        foreach ($v in 'PendingFileRenameOperations', 'PendingFileRenameOperations2') {
            $vals = @($sm.$v)
            "${v}: $($vals.Count) strings"
            for ($i = 0; $i -lt $vals.Count; $i++) { if ($vals[$i] -match 'RB Stems Plus') { "  [$i] '$($vals[$i])' then '$(if ($i + 1 -lt $vals.Count) { $vals[$i + 1] })'" } }
        }
        Check (Test-Path -LiteralPath $CacheRoot) 'the root stays until the restart'
        $stop = Join-Path $UacDir 'hold.stop'
        Set-Content -LiteralPath $stop -Value 'stop'
        Check (Wait-Until { (Read-Shared (Join-Path $UacDir 'hold.txt')) -match 'released' } 30) 'the hold released'
        Unregister-ScheduledTask -TaskName 'rbsp-ui-hold' -Confirm:$false -ErrorAction SilentlyContinue
    }
    '^j7-restarted' {
        Check ((Get-Sha (Join-Path $dir 'RB Stems Plus.exe')) -eq $admin) "J7: the bin\ worker is there after the restart, the payload's ($dir)"
        Check ($dels.Count -eq 0) "no *.del left ($($dels -join ', '))"
    }
    '^root-gone' { Check (-not (Test-Path -LiteralPath $CacheRoot)) "R4.I77: Stems Cache's root is gone after the restart ($CacheRoot)" }
    default { Check $false "no case for a step named $($VmRun.Step)" }
}
Done
