# sslib.ps1: sessions.suite's own helpers, after lib/vmlib.ps1 and uninstall/cases/unlib.ps1: UN8's
# second account B, the folder of the task that runs as B (its script, what it reports, its stop),
# and B's processes.
$BName = 'rbB'
$BDir = "C:\rbtest\job\$($VmRun.Run)\as-$BName"
$BTask = "rbstems-as-$BName-$($VmRun.Run)"

# B's SID, $null when there is no such account
function Get-BSid {
    try { return (New-Object Security.Principal.NTAccount($BName)).Translate([Security.Principal.SecurityIdentifier]).Value } catch { return $null }
}
# B's profile folder (ProfileList), $null before B's first logon
function Get-BProfile {
    $sid = Get-BSid
    if (-not $sid) { return $null }
    $k = Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\$sid" -ErrorAction SilentlyContinue
    if ($k) { return $k.ProfileImagePath }
    return $null
}
# as SYSTEM: the processes running from B's apps folder (B's app, B's watcher), by full path
function Get-BProcesses {
    $p = Get-BProfile
    if (-not $p) { return @() }
    $apps = Join-Path $p 'AppData\Local\Programs\RB Stems Plus\'
    return @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($apps, [StringComparison]::OrdinalIgnoreCase) })
}
# as the user (A): the processes by our image names (RB Stems Plus, rbsp-watch-*) that this token
# can't open (OpenProcess's error, never 0), with their session
function Get-RefusedOurs {
    Add-VmNative
    return @(Get-Process -Name 'RB Stems Plus', 'rbsp-watch-*' -ErrorAction SilentlyContinue | ForEach-Object {
            [pscustomobject]@{ Id = $_.Id; Name = $_.ProcessName; Session = $_.SessionId; Error = [RbspVm.Native]::OpenError($_.Id) } } |
            Where-Object { $_.Error -ne 0 })
}

# The desktop variant requires exactly one Explorer owned by rbB, in a nonzero session.
function Get-BDesktop {
    $sid = Get-BSid
    if (-not $sid) { return $null }
    $sessions = @(Get-CimInstance Win32_Process -Filter "Name = 'explorer.exe'" | Where-Object {
        $_.SessionId -ne 0 -and (Invoke-CimMethod -InputObject $_ -MethodName GetOwnerSid).Sid -eq $sid
    } | Select-Object -ExpandProperty SessionId -Unique)
    if ($sessions.Count -eq 1) { return $sessions[0] }
    return $null
}
