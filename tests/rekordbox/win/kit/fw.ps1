# rekordbox's network blocked by Windows Firewall for the test runs. Not used now: the owner declined
# the firewall (PC session 1); -Mode Check (read-only, no elevation) is the connection watch.
# Add/Enable/Disable/Remove run elevated (the owner's admin SSH), only with the owner's OK. Rules in the group "RBSP test", inbound and outbound,
# every profile, for rekordbox 7.2.19's network-capable programs. Machine-wide: the owner's
# account's rekordbox is blocked too while they are on.
#   -Mode Check    read-only: the group's rules, the profiles, and any non-loopback TCP connection
#                  of a running rekordbox process (there should be none while blocked)
#   -Mode Add      add the rules (-WhatIf: say what would be added, change nothing)
#   -Mode Disable  turn the group off (kept for later); -Mode Enable turns it on again
#   -Mode Remove   delete the group's rules
param([Parameter(Mandatory = $true)][ValidateSet('Check', 'Add', 'Enable', 'Disable', 'Remove')][string]$Mode, [switch]$WhatIf,
      [ValidatePattern('^[0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}$')][string]$RbVersion)
$ErrorActionPreference = 'Stop'
$Group = 'RBSP test'
. (Join-Path $PSScriptRoot 'kit-lib.ps1')
$rb = Get-Rb; if (-not $rb) { 'no rekordbox installed'; exit 1 }
$Dir = $rb.Dir
$Progs = 'rekordbox.exe', 'rekordboxAgent-win32-x64\rekordboxAgent.exe', 'edb_streamd.exe', 'rbHttpServer.exe', 'Upmgr rekordbox.exe', 'rbinit.exe'
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) -and $Mode -ne 'Check') { 'REFUSED: run elevated'; exit 2 }
switch ($Mode) {
'Add' {
    foreach ($p in $Progs) {
        $full = Join-Path $Dir $p
        if (-not (Test-Path -LiteralPath $full)) { "missing: $full"; exit 1 }
        foreach ($d in 'Outbound', 'Inbound') {
            $name = "RBSP test: block $d $(Split-Path -Leaf $p)"
            if (Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue) { "already there: $name"; continue }
            New-NetFirewallRule -DisplayName $name -Group $Group -Direction $d -Action Block -Program $full -Profile Any -Enabled True -WhatIf:$WhatIf | Out-Null
            "$(if ($WhatIf) { 'would add' } else { 'added' }): $name"
        }
    }
}
'Enable' { Enable-NetFirewallRule -Group $Group; "enabled: the group $Group" }
'Disable' { Disable-NetFirewallRule -Group $Group; "disabled: the group $Group" }
'Remove' { Remove-NetFirewallRule -Group $Group; "removed: the group $Group" }
}
# Check (after every mode)
$r = @(Get-NetFirewallRule -Group $Group -ErrorAction SilentlyContinue)
"the group $Group : $($r.Count) rules, $(@($r | Where-Object { $_.Enabled -eq 'True' }).Count) enabled"
foreach ($x in $r) { "  $($x.DisplayName): $($x.Direction) $($x.Action) enabled $($x.Enabled) profile $($x.Profile)" }
foreach ($p in Get-NetFirewallProfile) { "profile $($p.Name): firewall on $($p.Enabled)" }
$procs = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($Dir, [StringComparison]::OrdinalIgnoreCase) })
"rekordbox processes running: $($procs.Count)"
foreach ($c in Get-NetTCPConnection -ErrorAction SilentlyContinue | Where-Object { $procs.Id -contains $_.OwningProcess -and $_.RemoteAddress -notmatch '^(127\.|::1$|0\.0\.0\.0$|::$)' -and $_.State -ne 'Listen' }) {
    "  NON-LOOPBACK: $(($procs | Where-Object Id -eq $c.OwningProcess).ProcessName) $($c.State) $($c.RemoteAddress):$($c.RemotePort)"
}
