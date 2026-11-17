# Part A2: the one-liner in an elevated window (the scheduled task's highest run level): the
# administrator refusal, so Test-RbspAdmin still runs in FullLanguage; nothing installed
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$admin = (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { Skip 'this user has no administrator token: the elevated case needs an administrator account' }
$l = Get-Layout
$had = Test-Path -LiteralPath $l.Apps
$out = Invoke-OneLiner $VmRun.Url
"got: $out"
$sac = Get-BootText 'SacOn'
Check ($out -ceq (Get-BootText 'Elevated') -or $out -ceq (Get-BootText 'UacOff') -or $out -ceq $sac) 'the administrator (or UAC off, or SAC) refusal alone'
Check ((Test-Path -LiteralPath $l.Apps) -eq $had) 'nothing installed'
Done
