# As SYSTEM, after worker/prep.ps1 (UAC elevates without a prompt; 7.2.19's facts): snapshot (b)'s
# facts (mv-vm-snapshots.md): 7.2.19 alone in Program Files with its audited hashes, 7.2.20 not
# installed, its real installer staged in Downloads with its hash; the served folder holds both
# manifests; and auditing of process creations turned on (Security log 4688, with the creator's
# name and the token's elevation), so each batch's elevations can be counted.
$p = Read-TestPayload
Check ((@($p.windows.compatible.rekordbox) -join ',') -eq '7.2.19') "the served root is M1: windows list $(@($p.windows.compatible.rekordbox) -join ', ')"
foreach ($m in 'm1', 'm2') {
    $f = Share "$MvServe/$m/payload.json"
    $j = if (Test-Path -LiteralPath $f) { Get-Content -Raw -LiteralPath $f | ConvertFrom-Json } else { $null }
    Check ($null -ne $j) "the served folder's $m/ ($f)"
    if ($j) { "  $m payload_version $($j.payload_version), windows list $(@($j.windows.compatible.rekordbox) -join ', '), ort_code_sha256 $($j.windows.compatible.ort_code_sha256)" }
}
Check ((Get-Sha (Get-RbExeOf '7.2.19')) -eq $MvExeSha['7.2.19']) "rekordbox 7.2.19's rekordbox.exe is the audited one"
Check ((Get-Sha (Get-RbOrtOf '7.2.19')) -eq $MvOrtSha['7.2.19']) "rekordbox 7.2.19's onnxruntime.dll is Pioneer's (e7a702e1...)"
Check (-not (Test-Path -LiteralPath (Get-RbDirOf '7.2.20'))) 'rekordbox 7.2.20 is not installed yet'
$user = (Get-CimInstance Win32_ComputerSystem).UserName
$inst = "C:\Users\$($user.Split('\')[-1])\Downloads\Install_rekordbox_x64_7_2_20.exe"
Check ((Get-Sha $inst) -eq $MvInstaller) "the real 7.2.20 installer is staged with its hash ($inst)"
Check (-not (Test-Path -LiteralPath (Join-Path (Get-UserProfile) 'AppData\Roaming\Pioneer\rekordbox\master.db'))) 'rekordbox was never started here (no master.db)'
# Process Creation (its GUID: the name is the system language's), and the command line in 4688
$out = & auditpol.exe /set /subcategory:'{0CCE922B-69AE-11D9-BED3-505054503030}' /success:enable 2>&1
"auditpol: $out"
Set-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System\Audit' -Name ProcessCreationIncludeCmdLine_Enabled -Value 1 -Type DWord -ErrorAction SilentlyContinue
$get = (& auditpol.exe /get /subcategory:'{0CCE922B-69AE-11D9-BED3-505054503030}' /r 2>&1 | Out-String)
Check ($get -match ',Success') "process creation auditing is on ($(($get -split "`n" | Select-Object -Last 2) -join ' '))"
Remove-Item -LiteralPath $MvDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $MvDir | Out-Null
Done
