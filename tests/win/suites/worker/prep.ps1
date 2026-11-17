# worker.suite's first step, as SYSTEM: the snapshot's facts the later steps count on, and
# rekordbox's own onnxruntime.dll's hash saved for them (C:\rbtest\worker\facts.json); then UAC
# set to elevate an administrator without a prompt (ConsentPromptBehaviorAdmin 0, read at each
# elevation) in this VM, which the run reverts at its end. UAC stays on (EnableLUA 1): the worker
# still starts through runas as an elevation, from the app's filtered token; only its prompt,
# which nothing answers in a run, is left out (the prompt's cases are by hand, by-hand.md). A run
# with --keep leaves ConsentPromptBehaviorAdmin 0 until the next run's revert (by design: --keep
# neither stops nor reverts).
$sac = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' -ErrorAction SilentlyContinue).VerifiedAndReputablePolicyState
Check ($sac -ne 1) "Smart App Control isn't On (state $sac): the unsigned app and hidden.exe's user steps run"
$pol = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System'
$lua = (Get-ItemProperty $pol).EnableLUA
Check ($lua -eq 1) "UAC is on (EnableLUA $lua)"
$name = (Get-CimInstance Win32_ComputerSystem).UserName
Check ([bool]$name) "a user is logged in ($name)"
$admin = $false
if ($name) {
    $sid = (New-Object Security.Principal.NTAccount($name)).Translate([Security.Principal.SecurityIdentifier]).Value
    # the group by its SID (its name is the system language's)
    $group = (New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')).Translate([Security.Principal.NTAccount]).Value.Split('\')[-1]
    $members = @(([ADSI]"WinNT://./$group,group").psbase.Invoke('Members') | ForEach-Object {
            $b = [byte[]]$_.GetType().InvokeMember('objectSid', 'GetProperty', $null, $_, $null)
            (New-Object Security.Principal.SecurityIdentifier($b, 0)).Value })
    $admin = $members -contains $sid
}
Check $admin "the user is an administrator (Stems Cache is for admin accounts only, app.md 5.5)"
Check (Test-Path -LiteralPath $RbExe) "rekordbox 7.2.19 is installed ($RbExe)"
$ver = if (Test-Path -LiteralPath $RbExe) { (Get-Item -LiteralPath $RbExe).VersionInfo.ProductVersion } else { '' }
Check ($ver -like '7.2.19*') "its ProductVersion is 7.2.19 ($ver)"
$sig = if (Test-Path -LiteralPath $RbOrt) { Get-AuthenticodeSignature -LiteralPath $RbOrt } else { $null }
Check ($sig -and $sig.Status -eq 'Valid') "rekordbox's onnxruntime.dll is Pioneer's, its signature valid ($(if ($sig) { "$($sig.Status), $($sig.SignerCertificate.Subject)" }))"
Check (-not (Test-Path -LiteralPath $CacheRoot)) "no Stems Cache here yet (no $CacheRoot)"
$p = Read-TestPayload
$bridge = [string]$p.windows.bridge.sha256
$worker = [string]$p.windows.app.admin_sha256
Check ($bridge -match '^[0-9a-fA-F]{64}$') "the TEST payload names the bridge's sha256 ($bridge)"
Check ($worker -match '^[0-9a-fA-F]{64}$') "the TEST payload names the worker's admin_sha256 ($worker)"
$bfile = [string]$p.windows.bridge.file
Check ($bfile -match '^[A-Za-z0-9._-]+$' -and (Test-Path -LiteralPath (Share "dist-test/$bfile"))) "the bridge it names is served (dist-test/$bfile)"
$compat = ConvertTo-Json -Compress -Depth 5 -InputObject @($p.windows.compatible.rekordbox)
Check ($compat -match '7\.2\.19') "the TEST payload lists rekordbox 7.2.19 for Stems Cache ($compat)"
New-Item -ItemType Directory -Force (Split-Path $WorkerFacts) | Out-Null
ConvertTo-Json -InputObject ([ordered]@{ PioneerSha = (Get-Sha $RbOrt); Version = $ver }) | Set-Content -LiteralPath $WorkerFacts -Encoding UTF8
"saved: $(Get-Content -Raw -LiteralPath $WorkerFacts)"
Set-ItemProperty -LiteralPath $pol -Name ConsentPromptBehaviorAdmin -Value 0 -Type DWord
Check ((Get-ItemProperty $pol).ConsentPromptBehaviorAdmin -eq 0) 'UAC elevates an administrator without a prompt now (ConsentPromptBehaviorAdmin 0, until the revert)'
Done
