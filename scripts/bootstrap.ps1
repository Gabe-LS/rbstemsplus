# RB Stems Plus installer for Windows: puts the app in %LOCALAPPDATA%\Programs\RB Stems Plus\,
# with its start stub, Start menu shortcut, background watcher and Apps & features entry, then
# opens it. Everything else happens in the app. Started by pasting into a normal (not
# administrator) PowerShell window:
#
#   irm https://github.com/Gabe-LS/rbstemsplus/releases/latest/download/bootstrap.ps1 | iex
#
# What it does, in order (release.md section 4; app.md section 5.4, W12):
#   1. refuses Smart App Control On, a language mode other than FullLanguage, Windows older than
#      11 22H2 or .NET Framework older than 4.8.1, an elevated window, and User Account Control off;
#   2. downloads payload.json and its signature and stops unless it verifies with one of the
#      public keys below (ECDSA P-256), no key in it is given twice, its version has the right
#      shape (x.y.z, no suffix) and isn't older than PAYLOAD_MINIMUM, and it
#      has the windows block;
#   3. downloads the zip the windows block names, checks its SHA-256, checks its entry names
#      (unzip.c's rules: no rooted name, '..', '\', ':', link entry), unpacks it entry by entry
#      into a staging folder with the sizes capped on the bytes written, and checks it: no link or
#      reparse point, the files the app needs, the worker's and both stubs' SHA-256, the app's
#      ProductVersion and ProductName (as the updater does);
#   4. stops a running background watcher (by its stop event), refuses while the app itself is
#      open, and moves the new version into its folder <version>\;
#   5. writes the start stub of this PC's architecture (checked against the payload), `current`
#      (the version the stub starts), the support folder's records the app's updater reads
#      (installed-release\ and payload\ with the payload pair, payload-version: the floor), the
#      Start menu shortcut with the app's AppUserModelID, the Run value that starts the watcher
#      at logon, and the Apps & features entry;
#   6. removes older version folders and opens the app through the stub.
# No UAC: everything goes into this account's own folders and HKCU.
#
# -Restore, the recovery that needs none of our binaries (release.md section 4): in an elevated
# powershell.exe, `$env:RBSTEMSPLUS_RESTORE='1'; irm <url> | iex` puts Pioneer's onnxruntime.dll
# back into rekordbox's folder from the copy the worker saved (Invoke-RbspRestore). It skips the
# Smart App Control and language stops and requires elevation; cmdlets only, so it runs in
# ConstrainedLanguage and with Smart App Control On.
#
# Rules: the whole script runs inside & { } and leaves by return or throw, never exit (under iex,
# exit would close the window). Pure ASCII, no BOM (tests/win/bootstrap checks it). Everything
# that runs before the refusals (the top level and Get-RbspFacts) is ConstrainedLanguage-safe:
# only core types, no [ref], no .NET method call, no New-Object or Add-Type, so a constrained
# window gets the language refusal, not an error.
#
# The limit: this script itself is trusted as pasted, over GitHub's HTTPS. The signature
# protects what it downloads, not the script.
#
# Test override: RBSTEMSPLUS_BASE_URL replaces the release URL (a local test server).
& {
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# The release everything is downloaded from: always the latest (no beta channel).
$BaseUrl = 'https://github.com/Gabe-LS/rbstemsplus/releases/latest/download/'
# The public keys payload.json's signature is checked with (base64 of their DER, space-separated),
# and the oldest payload_version installed (this release's VERSION): written in by
# scripts/build.sh. Empty here: run only the copy a build made.
$SigningKeys = ''
$PayloadMinimum = ''

$AppName = 'RB Stems Plus'
$AppExe = 'RB Stems Plus.exe'
$Aumid = 'io.github.rbstemsplus.app'   # win/c/aumid.h's RBSP_AUMID
$Publisher = 'Gabe-LS'
$UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\RBStemsPlus'
$RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run'
$RunValue = 'RB Stems Plus Watcher'
# The install lock (win/c/afterexit.h's AX_INSTALL_LOCK): an uninstall's after-exit deletes nothing
# while this run holds it, and holds it itself while it deletes (audit b-a2b-F2)
$InstallLock = 'Local\io.github.rbstemsplus.install'

# The lines this script can say (app.md W14a's table of Windows texts). The Smart App Control and
# old-Windows ones are the core's T_WIN_SAC_ON and T_WIN_START_OLD_WINDOWS (texts.def); U+203A
# is written as a char so this file stays ASCII.
$Gt = [string][char]0x203A
$Texts = @{
    SacOn        = "Smart App Control is on, so RB Stems Plus can't be installed. Turn it off in Windows Security $Gt App & browser control $Gt Smart App Control, then run the install command again."
    Language     = "RB Stems Plus can't be installed from this PowerShell window. Open a normal PowerShell window, then run the install command again."
    OldWindows   = 'RB Stems Plus needs Windows 11, version 22H2 or later.'
    OldNet       = 'RB Stems Plus needs .NET Framework 4.8.1 or later. Install it with Windows Update, then run the install command again.'
    Elevated     = 'Run the install command in a normal PowerShell window, not as administrator.'
    UacOff       = "RB Stems Plus can't be installed while User Account Control is off. Turn it on, restart Windows, then run the install command again."
    AppOpen      = 'RB Stems Plus is open. Quit it, then run the install command again.'
    Waiting      = "Waiting for RB Stems Plus's uninstall to finish..."
    Uninstalling = 'RB Stems Plus is still being uninstalled. Try again in a minute.'
    NotWindows   = 'This release has no Windows version yet.'
    Failed       = "RB Stems Plus wasn't installed: "
    RestoreElevated = 'Run the restore command in PowerShell as administrator.'
    RestoreOpen     = 'rekordbox is open. Quit it, then run the restore command again.'
    RestoreBusy     = 'Finish updating or uninstalling rekordbox, or wait for RB Stems Plus to finish, then run the restore command again.'
    RestoreNone     = "There is no saved copy of rekordbox's file to put back. Reinstall rekordbox with Pioneer's installer."
    RestoreFailed   = "rekordbox's file wasn't put back: "
    # -Restore's reasons with a folder, a file or a version in them ({0}, {1}: -f)
    RestoreLinked   = '{0} is reached through a link'
    RestoreOthers   = '{0} can be changed by other accounts'
    RestoreNotFile  = "rekordbox {0}'s ONNX Runtime isn't a file"
    RestoreGone     = 'rekordbox {0} has no ONNX Runtime file now: run the restore command again ({1})'
}

# ------------------------------------------------------------------ versions

function Test-RbspReleaseVersion([string]$s) { return $s -cmatch '^[0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}\z' }

# The order of two x.y.z versions: -1, 0, 1; 2 when either isn't one. Parts compare as numbers
# (payload.c's payload_version_cmp).
function ConvertTo-RbspVersionParts([string]$s) {
    if (-not (Test-RbspReleaseVersion $s)) { return $null }
    return ,@($s -split '\.' | ForEach-Object { [long]$_ })
}
function Compare-RbspVersion([string]$a, [string]$b) {
    $va = ConvertTo-RbspVersionParts $a
    $vb = ConvertTo-RbspVersionParts $b
    if ($null -eq $va -or $null -eq $vb) { return 2 }
    for ($i = 0; $i -lt 3; $i++) {
        if ($va[$i] -ne $vb[$i]) { return $(if ($va[$i] -lt $vb[$i]) { -1 } else { 1 }) }
    }
    return 0
}

# ------------------------------------------------------------------ the signature

# Strict DER ECDSA-Sig-Value of P-256 into r||s (64 bytes), or $null (sig.c's sig_der_to_rs).
function ConvertFrom-RbspDerSignature([byte[]]$d) {
    if ($null -eq $d -or $d.Length -lt 8 -or $d.Length -gt 72 -or $d[0] -ne 0x30 -or $d[1] -ne $d.Length - 2) { return $null }
    $rs = New-Object byte[] 64
    $at = 2
    for ($k = 0; $k -lt 2; $k++) {
        if ($at + 2 -gt $d.Length -or $d[$at] -ne 0x02) { return $null }
        $len = [int]$d[$at + 1]
        $at += 2
        if ($len -lt 1 -or $len -gt 33 -or $at + $len -gt $d.Length) { return $null }
        if ($d[$at] -band 0x80) { return $null }                                  # negative
        if ($len -gt 1 -and $d[$at] -eq 0 -and -not ($d[$at + 1] -band 0x80)) { return $null }   # not minimal
        $start = $at; $n = $len
        if ($d[$at] -eq 0 -and $len -gt 1) { $start++; $n-- }
        if ($n -gt 32) { return $null }
        $allzero = $true
        for ($j = 0; $j -lt $n; $j++) { if ($d[$start + $j] -ne 0) { $allzero = $false } }
        if ($allzero) { return $null }                                            # zero
        [Array]::Copy($d, $start, $rs, $k * 32 + 32 - $n, $n)
        $at += $len
    }
    if ($at -ne $d.Length) { return $null }
    return ,$rs
}

# The DER SubjectPublicKeyInfo header of a P-256 key with an uncompressed point (sig.c's)
$SpkiHeader = [byte[]](0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06,
                       0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04)

# The index of the first key (base64 of its SPKI) whose ECDSA P-256 signature over $data this
# is, or -1. ECDsa.Create with the point's X and Y, so the same code runs in PowerShell 5.1 and 7.
function Test-RbspPayloadSignature([byte[]]$data, [byte[]]$sig, [string[]]$keys) {
    $rs = ConvertFrom-RbspDerSignature $sig
    if ($null -eq $rs) { return -1 }
    for ($i = 0; $i -lt $keys.Count; $i++) {
        try { $spki = [Convert]::FromBase64String($keys[$i]) } catch { continue }
        if ($spki.Length -ne 91) { continue }
        $same = $true
        for ($j = 0; $j -lt 27; $j++) { if ($spki[$j] -ne $SpkiHeader[$j]) { $same = $false } }
        if (-not $same) { continue }
        $p = New-Object System.Security.Cryptography.ECParameters
        $p.Curve = [System.Security.Cryptography.ECCurve]::CreateFromFriendlyName('nistP256')
        $q = New-Object System.Security.Cryptography.ECPoint
        $q.X = [byte[]]$spki[27..58]
        $q.Y = [byte[]]$spki[59..90]
        $p.Q = $q
        try {
            $ec = [System.Security.Cryptography.ECDsa]::Create($p)
            if ($ec.VerifyData($data, $rs, [System.Security.Cryptography.HashAlgorithmName]::SHA256)) { return $i }
        } catch { continue }
    }
    return -1
}

# ------------------------------------------------------------------ payload.json

# Payload.swift's plainJSONKeys: one JSON object in which no object at any level has a key
# twice, and every key is non-empty printable ASCII without a backslash. Values only skipped.
function Test-RbspPlainJson([byte[]]$b) {
    $st = @{ i = 0 }
    $ws = { while ($st.i -lt $b.Length -and ($b[$st.i] -eq 0x20 -or $b[$st.i] -eq 0x09 -or $b[$st.i] -eq 0x0a -or $b[$st.i] -eq 0x0d)) { $st.i++ } }
    $at = { param($c) $st.i -lt $b.Length -and $b[$st.i] -eq $c }
    $str = {
        if (-not (& $at 0x22)) { return $null }
        $st.i++
        $start = $st.i
        while ($st.i -lt $b.Length) {
            $c = $b[$st.i]
            if ($c -eq 0x22) { $st.i++; return ,@($start, ($st.i - 1)) }
            if ($c -eq 0x5c) { $st.i += 2; continue }
            if ($c -lt 0x20) { return $null }
            $st.i++
        }
        return $null
    }
    $value = $null
    $value = {
        param($depth)
        & $ws
        if ($depth -ge 32 -or $st.i -ge $b.Length) { return $false }
        $c = $b[$st.i]
        if ($c -eq 0x7b) {
            $st.i++; & $ws
            if (& $at 0x7d) { $st.i++; return $true }
            $keys = New-Object 'System.Collections.Generic.HashSet[string]'
            while ($true) {
                & $ws
                $k = & $str
                if ($null -eq $k -or $k[1] -le $k[0]) { return $false }
                $sb = New-Object System.Text.StringBuilder
                for ($j = $k[0]; $j -lt $k[1]; $j++) {
                    $x = $b[$j]
                    if ($x -lt 0x20 -or $x -ge 0x7f -or $x -eq 0x5c) { return $false }
                    [void]$sb.Append([char]$x)
                }
                if (-not $keys.Add($sb.ToString())) { return $false }
                & $ws
                if (-not (& $at 0x3a)) { return $false }
                $st.i++
                if (-not (& $value ($depth + 1))) { return $false }
                & $ws
                if (& $at 0x2c) { $st.i++; continue }
                if (& $at 0x7d) { $st.i++; return $true }
                return $false
            }
        }
        if ($c -eq 0x5b) {
            $st.i++; & $ws
            if (& $at 0x5d) { $st.i++; return $true }
            while ($true) {
                if (-not (& $value ($depth + 1))) { return $false }
                & $ws
                if (& $at 0x2c) { $st.i++; continue }
                if (& $at 0x5d) { $st.i++; return $true }
                return $false
            }
        }
        if ($c -eq 0x22) { return $null -ne (& $str) }
        $start = $st.i
        $allowed = '+-.0123456789Eaeflnrstu'
        while ($st.i -lt $b.Length -and $allowed.IndexOf([char]$b[$st.i]) -ge 0) { $st.i++ }
        return $st.i -gt $start
    }
    & $ws
    if (-not (& $at 0x7b)) { return $false }
    if (-not (& $value 0)) { return $false }
    & $ws
    return $st.i -eq $b.Length
}

function Test-RbspSha256([object]$s) { return ($s -is [string]) -and ($s -cmatch '^[0-9a-f]{64}\z') }
function Test-RbspPlainName([object]$s) {
    return ($s -is [string]) -and ($s -cmatch '^[A-Za-z0-9._-]{1,128}\z') -and $s -ne '.' -and $s -ne '..'
}

# payload.json's parts this script uses, checked (payload.c's rules for them): the payload's
# version and the windows block's app. Throws with the reason; $null when there is no windows block.
function Read-RbspPayload([byte[]]$json) {
    if (-not (Test-RbspPlainJson $json)) { throw 'payload.json has a key twice, or a key that is not plain' }
    $text = (New-Object System.Text.UTF8Encoding($false, $true)).GetString($json)
    $m = ConvertFrom-Json -InputObject $text
    $pv = $m.payload_version
    if (-not (Test-RbspReleaseVersion $pv)) { throw "payload_version '$pv' isn't a version this script installs" }
    if (-not ($m.PSObject.Properties.Name -contains 'windows')) { return $null }
    $w = $m.windows
    $app = $w.app
    $v = $app.version
    if (-not (Test-RbspReleaseVersion $v)) { throw "windows.app.version '$v' isn't a version" }
    $c = Compare-RbspVersion $v $pv
    if ($c -eq 2 -or $c -gt 0) { throw "windows.app.version $v is newer than the payload $pv" }
    if (-not (Test-RbspPlainName $app.zip)) { throw 'windows.app.zip is not a plain file name' }
    foreach ($h in @($app.sha256, $app.admin_sha256, $app.stub_sha256.x64, $app.stub_sha256.arm64)) {
        if (-not (Test-RbspSha256 $h)) { throw 'a windows.app SHA-256 is not 64 lowercase hex digits' }
    }
    # the rest of the windows block, as far as payload.c requires it to be there (the app reads it)
    $names = $w.PSObject.Properties.Name
    if (-not ($names -contains 'bridge') -or -not ($w.bridge -is [pscustomobject]) -or
        -not ($w.bridge.PSObject.Properties.Name -contains 'file') -or -not ($w.bridge.file -is [string])) { throw 'windows.bridge is missing or has no file' }
    if (-not ($names -contains 'compatible') -or -not ($w.compatible -is [pscustomobject])) { throw 'windows.compatible is missing' }
    return [pscustomobject]@{
        PayloadVersion = $pv; AppVersion = $v; Zip = $app.zip; ZipSha256 = $app.sha256
        AdminSha256 = $app.admin_sha256; StubX64 = $app.stub_sha256.x64; StubArm64 = $app.stub_sha256.arm64
    }
}

# ------------------------------------------------------------------ the layout and the records

# Where everything goes (app.md section 5.10): the app folder with the stub, `current` and the
# version folders; the support folder with the updater's records; the Start menu shortcut.
function Get-RbspLayout([string]$localAppData, [string]$programs) {
    $apps = [IO.Path]::Combine($localAppData, 'Programs', $AppName)
    $support = [IO.Path]::Combine($localAppData, 'rbstemsplus')
    return [pscustomobject]@{
        Apps = $apps
        Stub = [IO.Path]::Combine($apps, $AppExe)
        Current = [IO.Path]::Combine($apps, 'current')
        Support = $support
        Installed = [IO.Path]::Combine($support, 'installed-release')
        Payload = [IO.Path]::Combine($support, 'payload')
        Floor = [IO.Path]::Combine($support, 'payload-version')
        Shortcut = [IO.Path]::Combine($programs, "$AppName.lnk")
    }
}

# A file's bytes replaced whole: a temp name beside it, flushed, then moved over it
function Write-RbspFile([string]$path, [byte[]]$bytes) {
    $dir = [IO.Path]::GetDirectoryName($path)
    [void][IO.Directory]::CreateDirectory($dir)
    $tmp = "$path.$PID.tmp"
    try {
        $fs = New-Object IO.FileStream($tmp, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $fs.Write($bytes, 0, $bytes.Length); $fs.Flush($true) } finally { $fs.Dispose() }
        if ([IO.File]::Exists($path)) { [IO.File]::Replace($tmp, $path, [NullString]::Value) } else { [IO.File]::Move($tmp, $path) }
    } catch { try { [IO.File]::Delete($tmp) } catch { }; throw }
}

function Get-RbspAscii([string]$s) { return [Text.Encoding]::ASCII.GetBytes($s) }

# `current` (update.c's update_current_parse, stub.c's stub_current): the version, then "\n"
function Get-RbspCurrentBytes([string]$version) { return ,(Get-RbspAscii "$version`n") }

# The updater's records (update.h): installed-release\ and payload\ get the payload pair, the
# signature first (a stop between the two leaves a pair that doesn't verify, never a wrong one);
# payload-version, the floor, gets the payload's version (mac-core 6.3, I25). The pair
# installed-release\ held, when it verifies with $keys, goes to installed-release\prev\ first
# (update.c's write_installed: update.h's "kept by each write").
function Write-RbspRecords($layout, [byte[]]$json, [byte[]]$sig, [string]$payloadVersion, [string[]]$keys) {
    $oldJson = [IO.Path]::Combine($layout.Installed, 'payload.json')
    $oldSig = [IO.Path]::Combine($layout.Installed, 'payload.json.sig')
    if ([IO.File]::Exists($oldJson) -and [IO.File]::Exists($oldSig)) {
        $oj = [IO.File]::ReadAllBytes($oldJson)
        $os = [IO.File]::ReadAllBytes($oldSig)
        if ((Test-RbspPayloadSignature $oj $os $keys) -ge 0) {
            $prev = [IO.Path]::Combine($layout.Installed, 'prev')
            Write-RbspFile ([IO.Path]::Combine($prev, 'payload.json.sig')) $os
            Write-RbspFile ([IO.Path]::Combine($prev, 'payload.json')) $oj
        }
    }
    foreach ($dir in @($layout.Installed, $layout.Payload)) {
        Write-RbspFile ([IO.Path]::Combine($dir, 'payload.json.sig')) $sig
        Write-RbspFile ([IO.Path]::Combine($dir, 'payload.json')) $json
    }
    Write-RbspFile $layout.Floor (Get-RbspAscii "$payloadVersion`n")
}

# The Run value's data: the stub with --watcher (app.md 5.7)
function Get-RbspRunCommand($layout) { return "`"$($layout.Stub)`" --watcher" }

# The Apps & features entry's values (release.md section 4): name -> @(kind, data)
function Get-RbspUninstallValues($layout, [string]$shownVersion, [long]$sizeKB) {
    return [ordered]@{
        DisplayName = @('String', $AppName)
        DisplayVersion = @('String', $shownVersion)
        Publisher = @('String', $Publisher)
        DisplayIcon = @('String', $layout.Stub)
        InstallLocation = @('String', $layout.Apps)
        UninstallString = @('String', "`"$($layout.Stub)`" --uninstall")
        NoModify = @('DWord', 1)
        NoRepair = @('DWord', 1)
        EstimatedSize = @('DWord', [int][Math]::Min($sizeKB, [int]::MaxValue))
    }
}

# ------------------------------------------------------------------ the unpacked app

# the files a Windows app zip must hold (release.md 3.1; update.c's `required`)
$Required = @($AppExe, 'x64/rbsp-core.dll', 'arm64/rbsp-core.dll', 'rbsp-watch-x64.exe', 'rbsp-watch-arm64.exe',
              "admin/$AppExe", "stub/x64/$AppExe", "stub/arm64/$AppExe", 'NOTICE', 'NOTICE-windows.txt', 'LICENSE')

# base, then rel's '/'-separated parts, joined with this system's separator
function Join-RbspPath([string]$base, [string]$rel) {
    $p = $base
    foreach ($part in $rel -split '/') { $p = [IO.Path]::Combine($p, $part) }
    return $p
}

function Get-RbspSha256([string]$path) { return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }

# Whether the unpacked tree at $dir is the app $p names: throws with the reason
function Test-RbspUnpacked([string]$dir, $p) {
    foreach ($item in Get-ChildItem -LiteralPath $dir -Recurse -Force) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "the download holds a link: $($item.Name)" }
    }
    foreach ($rel in $Required) {
        $f = Join-RbspPath $dir $rel
        if (-not [IO.File]::Exists($f)) { throw "the download lacks $rel" }
    }
    $hashed = @(@("admin/$AppExe", $p.AdminSha256), @("stub/x64/$AppExe", $p.StubX64), @("stub/arm64/$AppExe", $p.StubArm64))
    foreach ($h in $hashed) {
        $f = Join-RbspPath $dir $h[0]
        if ((Get-RbspSha256 $f) -ne $h[1]) { throw "$($h[0]) isn't the one the payload names" }
    }
}

# A zip entry's name as unzip.c's unzip_name_ok takes it: relative, '/'-separated, printable
# ASCII without \ : * ? " < > |, no empty, '.' or '..' part, no part ending in a space or a dot,
# no Windows device name (CON, PRN, AUX, NUL, COM0-9, LPT0-9, CONIN$, CONOUT$, any extension), at
# most 255 bytes and 8 parts; a folder's entry ends in '/'
function Test-RbspZipName([string]$n) {
    if ($n.Length -eq 0 -or $n.Length -gt 255 -or $n[0] -eq '/') { return $false }
    if ($n -cmatch '[^\x20-\x7e]' -or $n.IndexOfAny([char[]]'\:*?"<>|') -ge 0) { return $false }
    $end = if ($n.EndsWith('/')) { $n.Substring(0, $n.Length - 1) } else { $n }
    if ($end.Length -eq 0) { return $false }
    $parts = $end.Split('/')
    if ($parts.Count -gt 8) { return $false }
    foreach ($c in $parts) {
        if ($c.Length -eq 0 -or $c -eq '.' -or $c -eq '..' -or $c.EndsWith(' ') -or $c.EndsWith('.')) { return $false }
        $base = $c
        $dot = $c.IndexOf([char]'.')
        if ($dot -ge 0) { $base = $c.Substring(0, $dot) }
        # upper-cased invariantly, then matched case-sensitively: a case-insensitive match folds by the
        # culture (under tr-TR 'i' isn't 'I', so 'conin$' would pass)
        if ($base.TrimEnd(' ').ToUpperInvariant() -cmatch '^(CON|PRN|AUX|NUL|CONIN\$|CONOUT\$|COM[0-9]|LPT[0-9])\z') { return $false }
    }
    return $true
}

# The downloaded zip's entries, read before Expand-RbspZip unpacks anything (defence in depth: the
# zip's SHA-256 comes from the signed payload). unzip.c's rules, the ones the app's updater reads
# the same zip with: every name per Test-RbspZipName, no symbolic link (a Unix mode of 0xA000 in
# the external attributes' high half), no name twice or a file that is also a folder of another
# name (ignoring case), no folder entry with data, at most 4096 entries, and 128 MiB a file and
# 512 MiB in all by the sizes the zip declares. Expand-RbspZip then holds every entry to its
# declared size and the total to 512 MiB on the bytes it writes (BS2), which covers what unzip.c's
# inflated-size and overlap checks guard against; method, encryption and ZIP64 are left to .NET's
# reader. Throws with the reason.
function Test-RbspZip([string]$zip) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $z = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $entries = @($z.Entries)
        if ($entries.Count -gt 4096) { throw 'the download holds too many files' }
        $names = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        $files = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        $total = [long]0
        foreach ($e in $entries) {
            $n = $e.FullName
            if (-not (Test-RbspZipName $n)) { throw "the download holds a name that isn't plain: $n" }
            if ((($e.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000) { throw "the download holds a link: $n" }
            $isDir = $n.EndsWith('/')
            if ($isDir -and $e.Length -ne 0) { throw "the download holds a folder with data: $n" }
            if ($e.Length -gt 128MB) { throw "the download holds a file too large: $n" }
            $total += $e.Length
            if ($total -gt 512MB) { throw 'the download unpacks too large' }
            $bare = $n.TrimEnd('/')
            if (-not $names.Add($bare)) { throw "the download holds a name twice: $n" }
            if (-not $isDir) { [void]$files.Add($bare) }
        }
        # a file that is also a folder of another name ("a" and "a/b")
        foreach ($bare in $names) {
            $at = $bare.IndexOf([char]'/')
            while ($at -gt 0) {
                if ($files.Contains($bare.Substring(0, $at))) { throw "the download holds a file and a folder of the same name: $bare" }
                $at = $bare.IndexOf([char]'/', $at + 1)
            }
        }
    } finally { $z.Dispose() }
}

# The zip unpacked into $dest (a new folder) entry by entry, after Test-RbspZip (BS2, in place of
# Expand-Archive): each name checked again and joined under $dest by its '/' parts, each file
# created new (never over another) and copied with a cap of its declared size plus one byte, so no
# entry writes more than the zip declares (one whose data reads longer or shorter is refused;
# .NET's reader may itself stop a deflated entry at its declared size), and the bytes written in
# all kept under $maxTotal (512 MiB; overlapping entries count each time they are read). Throws
# with the reason; the caller removes the staging folder.
function Expand-RbspZip([string]$zip, [string]$dest, [long]$maxTotal = 512MB) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [void][IO.Directory]::CreateDirectory($dest)
    $z = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $buf = New-Object byte[] 65536
        $total = [long]0
        foreach ($e in @($z.Entries)) {
            $n = $e.FullName
            if (-not (Test-RbspZipName $n)) { throw "the download holds a name that isn't plain: $n" }
            $target = Join-RbspPath $dest $n.TrimEnd('/')
            if ($n.EndsWith('/')) { [void][IO.Directory]::CreateDirectory($target); continue }
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
            $want = [long]$e.Length
            $got = [long]0
            $in = $e.Open()
            try {
                $out = New-Object IO.FileStream($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
                try {
                    while ($got -le $want) {
                        $k = $in.Read($buf, 0, [int][Math]::Min([long]$buf.Length, $want + 1 - $got))
                        if ($k -le 0) { break }
                        $got += $k
                        if ($got -gt $want) { break }
                        $total += $k
                        if ($total -gt $maxTotal) { throw 'the download unpacks too large' }
                        $out.Write($buf, 0, $k)
                    }
                } finally { $out.Dispose() }
            } finally { $in.Dispose() }
            if ($got -ne $want) { throw "the download's $n isn't the size the zip declares" }
        }
    } finally { $z.Dispose() }
}

# The unpacked app's version resource names the app the payload does (update.c's check_app): its
# ProductVersion is windows.app.version (a "+build" suffix allowed), its ProductName RB Stems
# Plus. Read through $sys.ProductInfo (only Windows reads a PE's version resource). Throws.
function Test-RbspAppVersion([string]$dir, $p, $sys) {
    $info = & $sys.ProductInfo ([IO.Path]::Combine($dir, $AppExe))
    $v = ([string]$info.Version).Trim()
    $n = ([string]$info.Name).Trim()
    if (-not ($v -ceq $p.AppVersion -or $v.StartsWith($p.AppVersion + '+', [StringComparison]::Ordinal))) {
        throw "the app's version ($v) isn't the one the payload names ($($p.AppVersion))"
    }
    if ($n -cne $AppName) { throw "the app's ProductName isn't $AppName" }
}

# ------------------------------------------------------------------ the install

# Everything after the checked download, with what only Windows does in $sys (scriptblocks the
# tests replace): AppRunning(apps) -> bool, StopWatchers(apps), SetRegistry(key, name, kind, data),
# NewShortcut(lnk, target, aumid), Start(exe, args); and ProductInfo(exe) -> @{Version; Name},
# which Test-RbspAppVersion uses. $unpacked: the checked, unpacked app (moved
# into its version folder; its parent is this run's .bootstrap-<pid>\); $arch: 'x64' or 'arm64';
# $keys: the signing keys (installed-release\'s pair is kept in prev\ when it verifies).
# Once the watchers are stopped, a watcher is started again whatever happens, when a stub and
# `current` are there (watch_win.c's one-copy mutex makes a second one exit); the app itself only
# after a whole install.
function Install-RbspApp($layout, $p, [byte[]]$json, [byte[]]$sig, [string]$unpacked, [string]$arch, $sys, [string[]]$keys) {
    if (& $sys.AppRunning $layout.Apps) { throw $Texts.AppOpen }
    [void](& $sys.StopWatchers $layout.Apps)
    $done = $false
    try {
        # the app opened while the watcher stopped (a watcher can start it)
        if (& $sys.AppRunning $layout.Apps) { throw $Texts.AppOpen }
        [void][IO.Directory]::CreateDirectory($layout.Apps)
        # the version folder: the unpacked app moved in; one already there (a reinstall, or an
        # earlier try) is moved aside first, and back when the move in fails
        $target = [IO.Path]::Combine($layout.Apps, $p.AppVersion)
        $aside = $null
        if ([IO.Directory]::Exists($target)) {
            $aside = [IO.Path]::Combine([IO.Path]::GetDirectoryName($unpacked), 'old')
            [IO.Directory]::Move($target, $aside)   # fails cleanly while a file in it is held
        }
        try { [IO.Directory]::Move($unpacked, $target) }
        catch { if ($aside) { [IO.Directory]::Move($aside, $target) }; throw }
        # the stub of this PC's architecture, checked, written beside and moved over the old one
        $want = if ($arch -eq 'arm64') { $p.StubArm64 } else { $p.StubX64 }
        $src = [IO.Path]::Combine($target, 'stub', $arch, $AppExe)
        if ((Get-RbspSha256 $src) -ne $want) { throw 'the start stub isn''t the one the payload names' }
        if (-not [IO.File]::Exists($layout.Stub) -or (Get-RbspSha256 $layout.Stub) -ne $want) {
            Write-RbspFile $layout.Stub ([IO.File]::ReadAllBytes($src))
        }
        # current (the stub starts this version from now on), then the Run value and the Apps &
        # features entry (an install stopped after here can still be removed in Settings > Apps),
        # then the updater's records and the shortcut
        Write-RbspFile $layout.Current (Get-RbspCurrentBytes $p.AppVersion)
        & $sys.SetRegistry $RunKey $RunValue 'String' (Get-RbspRunCommand $layout)
        $size = 0
        foreach ($f in Get-ChildItem -LiteralPath $target -Recurse -File -Force) { $size += $f.Length }
        $values = Get-RbspUninstallValues $layout $p.PayloadVersion ([long][Math]::Ceiling($size / 1024))
        foreach ($name in $values.Keys) { & $sys.SetRegistry $UninstallKey $name $values[$name][0] $values[$name][1] }
        Write-RbspRecords $layout $json $sig $p.PayloadVersion $keys
        & $sys.NewShortcut $layout.Shortcut $layout.Stub $Aumid
        # older version folders and leftovers of this script
        foreach ($d in Get-ChildItem -LiteralPath $layout.Apps -Directory -Force) {
            if ($d.Name -ne $p.AppVersion -and ((Test-RbspReleaseVersion $d.Name) -or $d.Name -like '.bootstrap-*')) {
                # this script's next run removes a .bootstrap-* that stays; the app's next start sweeps old version folders
                try { [IO.Directory]::Delete($d.FullName, $true) } catch { }
            }
        }
        # This install has superseded the updater's attempt. Clear its records before either start.
        $update = [IO.Path]::Combine($layout.Support, 'update')
        # each on its own: a record that can't go now doesn't turn a finished install into a failure
        foreach ($name in @('pending', 'started', 'payload.json', 'payload.json.sig', 'watcher', 'result')) {
            try { [IO.File]::Delete([IO.Path]::Combine($update, $name)) } catch { }
        }
        $done = $true
    } finally {
        if ([IO.File]::Exists($layout.Stub) -and [IO.File]::Exists($layout.Current)) {
            try { & $sys.Start $layout.Stub @('--watcher') } catch { if ($done) { throw } }
        }
    }
    & $sys.Start $layout.Stub @()
}

# ------------------------------------------------------------------ Windows

# Smart App Control's state (0 off, 1 on, 2 evaluation), read so ConstrainedLanguage can too
function Get-RbspSacState {
    $k = 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy'
    $v = Get-ItemProperty -LiteralPath $k -Name VerifiedAndReputablePolicyState -ErrorAction SilentlyContinue
    if ($null -eq $v) { return 0 }
    return [int]$v.VerifiedAndReputablePolicyState
}

function Get-RbspNativeArch {
    $v = Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment' -Name PROCESSOR_ARCHITECTURE
    if ($v.PROCESSOR_ARCHITECTURE -eq 'ARM64') { return 'arm64' }
    return 'x64'
}

$ShortcutCode = @'
using System;
using System.Runtime.InteropServices;
public static class RbspShortcut {
    [ComImport, Guid("000214F9-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IShellLinkW {
        void GetPath(IntPtr a, int b, IntPtr c, int d); void GetIDList(out IntPtr p); void SetIDList(IntPtr p);
        void GetDescription(IntPtr a, int b); void SetDescription([MarshalAs(UnmanagedType.LPWStr)] string s);
        void GetWorkingDirectory(IntPtr a, int b); void SetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)] string s);
        void GetArguments(IntPtr a, int b); void SetArguments([MarshalAs(UnmanagedType.LPWStr)] string s);
        void GetHotkey(out short h); void SetHotkey(short h); void GetShowCmd(out int c); void SetShowCmd(int c);
        void GetIconLocation(IntPtr a, int b, out int c); void SetIconLocation([MarshalAs(UnmanagedType.LPWStr)] string s, int i);
        void SetRelativePath([MarshalAs(UnmanagedType.LPWStr)] string s, int r); void Resolve(IntPtr h, int f);
        void SetPath([MarshalAs(UnmanagedType.LPWStr)] string s);
    }
    [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPropertyStore {
        void GetCount(out uint c); void GetAt(uint i, out PropertyKey k); void GetValue(ref PropertyKey k, IntPtr v);
        void SetValue(ref PropertyKey k, ref PropVariant v); void Commit();
    }
    [ComImport, Guid("0000010b-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPersistFile {
        void GetClassID(out Guid g); [PreserveSig] int IsDirty(); void Load([MarshalAs(UnmanagedType.LPWStr)] string f, int m);
        void Save([MarshalAs(UnmanagedType.LPWStr)] string f, bool remember); void SaveCompleted([MarshalAs(UnmanagedType.LPWStr)] string f);
        void GetCurFile(out IntPtr f);
    }
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct PropertyKey { public Guid fmtid; public uint pid; }
    [StructLayout(LayoutKind.Explicit)]
    public struct PropVariant { [FieldOffset(0)] public ushort vt; [FieldOffset(8)] public IntPtr p; }
    [ComImport, Guid("00021401-0000-0000-C000-000000000046")] class ShellLink { }

    // the .lnk at path, starting target, with the AppUserModelID (PKEY_AppUserModel_ID)
    public static void Create(string path, string target, string aumid) {
        IShellLinkW link = (IShellLinkW)new ShellLink();
        link.SetPath(target);
        link.SetWorkingDirectory(System.IO.Path.GetDirectoryName(target));
        link.SetIconLocation(target, 0);
        IPropertyStore store = (IPropertyStore)link;
        PropertyKey key = new PropertyKey();
        key.fmtid = new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3");
        key.pid = 5;
        PropVariant v = new PropVariant();
        v.vt = 31;   // VT_LPWSTR
        v.p = Marshal.StringToCoTaskMemUni(aumid);
        try { store.SetValue(ref key, ref v); store.Commit(); } finally { Marshal.FreeCoTaskMem(v.p); }
        ((IPersistFile)link).Save(path, true);
    }
}
'@

# what only Windows does (Install-RbspApp's $sys)
function Get-RbspWindowsSys {
    return @{
        AppRunning = {
            param($apps)
            foreach ($pr in Get-Process -ErrorAction SilentlyContinue) {
                $path = $null
                try { $path = $pr.Path } catch { }
                if ($path -and $path.StartsWith("$apps\", [StringComparison]::OrdinalIgnoreCase) -and
                    [IO.Path]::GetFileName($path) -eq $AppExe -and [IO.Path]::GetDirectoryName($path) -ne $apps) { return $true }
            }
            return $false
        }
        StopWatchers = {
            param($apps)
            $found = $false
            foreach ($pr in Get-Process -ErrorAction SilentlyContinue) {
                $path = $null
                try { $path = $pr.Path } catch { }
                if (-not $path -or -not $path.StartsWith("$apps\", [StringComparison]::OrdinalIgnoreCase) -or
                    -not [IO.Path]::GetFileName($path).StartsWith('rbsp-watch', [StringComparison]::OrdinalIgnoreCase)) { continue }
                $found = $true
                try {
                    $ev = [Threading.EventWaitHandle]::OpenExisting("Local\io.github.rbstemsplus.watcher.stop.$($pr.Id)")
                    [void]$ev.Set(); $ev.Dispose()
                } catch { }
                if (-not $pr.WaitForExit(10000)) { try { $pr.Kill(); [void]$pr.WaitForExit(5000) } catch { } }
            }
            return $found
        }
        SetRegistry = {
            param($key, $name, $kind, $data)
            $k = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($key)
            try {
                $rk = if ($kind -eq 'DWord') { [Microsoft.Win32.RegistryValueKind]::DWord } else { [Microsoft.Win32.RegistryValueKind]::String }
                $k.SetValue($name, $data, $rk)
            } finally { $k.Dispose() }
        }
        ProductInfo = {
            param($exe)
            $vi = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe)
            return @{ Version = [string]$vi.ProductVersion; Name = [string]$vi.ProductName }
        }
        NewShortcut = {
            param($lnk, $target, $aumid)
            if (-not ('RbspShortcut' -as [type])) { Add-Type -TypeDefinition $ShortcutCode -Language CSharp }
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($lnk))
            [RbspShortcut]::Create($lnk, $target, $aumid)
        }
        Start = {
            param($exe, $argv)
            if ($argv.Count) { Start-Process -FilePath $exe -ArgumentList $argv -WorkingDirectory ([IO.Path]::GetDirectoryName($exe)) }
            else { Start-Process -FilePath $exe -WorkingDirectory ([IO.Path]::GetDirectoryName($exe)) }
        }
    }
}

# Whether this window's token is an administrator's. .NET calls on non-core types: FullLanguage
# only (Get-RbspFacts calls it only there)
function Test-RbspAdmin {
    try {   # only off Windows does this throw, and the build refuses there first
        $id = [Security.Principal.WindowsIdentity]::GetCurrent()
        $principal = New-Object Security.Principal.WindowsPrincipal($id)
        return [bool]$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    } catch { return $false }
}

# What the refusals before any download look at: Smart App Control's state, the language mode,
# the Windows build and the .NET Framework release (0 when unread), whether UAC is off (EnableLUA
# 0), whether this window's token is an administrator's. Runs in any language mode, so the
# refusals can be said: cmdlets, property reads and casts to core types only (no [ref], no
# method call, no New-Object, no Add-Type; tests/win/bootstrap checks the text). The
# administrator check is skipped outside FullLanguage, which is refused anyway.
function Get-RbspFacts {
    $language = [string]$ExecutionContext.SessionState.LanguageMode
    $nt = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -Name CurrentBuildNumber -ErrorAction SilentlyContinue
    $build = 0
    if ($null -ne $nt -and [string]$nt.CurrentBuildNumber -match '^[0-9]{1,9}$') { $build = [int]$nt.CurrentBuildNumber }
    $net = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\NET Framework Setup\NDP\v4\Full' -Name Release -ErrorAction SilentlyContinue
    $lua = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' -Name EnableLUA -ErrorAction SilentlyContinue
    $admin = $false
    if ($language -eq 'FullLanguage') { $admin = Test-RbspAdmin }
    return @{
        Sac = Get-RbspSacState
        Language = $language
        Build = $build
        Net = $(if ($null -eq $net) { 0 } else { [int]$net.Release })
        UacOff = ($null -ne $lua -and [int]$lua.EnableLUA -eq 0)
        Admin = [bool]$admin
    }
}

# the refusals before any download (section 4's order: SAC first, then the language mode); an
# administrator's token is refused, and with UAC off that is the reason said (life_token_verdict)
function Test-RbspCanInstall($facts) {
    if ($facts.Sac -eq 1) { throw $Texts.SacOn }
    if ($facts.Language -ne 'FullLanguage') { throw $Texts.Language }
    if ($facts.Build -lt 22621) { throw $Texts.OldWindows }
    if ($facts.Net -lt 533320) { throw $Texts.OldNet }
    if ($facts.Admin) {
        if ($facts.UacOff) { throw $Texts.UacOff }
        throw $Texts.Elevated
    }
}

function Get-RbspDownload([string]$url, [string]$out) {
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $out -TimeoutSec 600
}

# Where the zip comes from: the release's own tag when payload.json came from the latest release
# (a release published between the two downloads would otherwise give another zip); else beside it
function Get-RbspZipUrl([string]$base, $p) {
    if ($base -ceq 'https://github.com/Gabe-LS/rbstemsplus/releases/latest/download/') {
        return "https://github.com/Gabe-LS/rbstemsplus/releases/download/v$($p.PayloadVersion)/$($p.Zip)"
    }
    return $base + $p.Zip
}

# The install lock taken (a [Threading.Mutex]; FullLanguage only, after the refusals): waits at
# most $ms while an uninstall's after-exit holds it (its 60 s wait for other programs and the
# deletes after it; 200 s), then refuses with nothing made. An abandoned one (a run that was
# killed) is ours.
function Open-RbspInstallLock([string]$name, [int]$ms) {
    $m = New-Object Threading.Mutex($false, $name)
    $got = $false
    try {
        $got = $m.WaitOne(0)
        if (-not $got) {
            $clock = [Diagnostics.Stopwatch]::StartNew()
            Write-Host $Texts.Waiting
            while (-not $got) {
                $left = $ms - $clock.ElapsedMilliseconds
                if ($left -le 0) { break }
                $got = $m.WaitOne([int][Math]::Min(500, $left))
            }
        }
    } catch [Threading.AbandonedMutexException] { $got = $true }
    finally { if (-not $got) { $m.Dispose() } }
    if (-not $got) { throw $Texts.Uninstalling }
    return $m
}

function Close-RbspInstallLock($m) {
    try { $m.ReleaseMutex() } catch { }
    $m.Dispose()
}

function Invoke-RbspMain {
    Test-RbspCanInstall (Get-RbspFacts)
    if (-not $SigningKeys -or -not $PayloadMinimum) { throw 'this copy of the script has no keys: run the one a release published' }
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $base = $BaseUrl
    if ($env:RBSTEMSPLUS_BASE_URL) { $base = $env:RBSTEMSPLUS_BASE_URL }
    if (-not $base.EndsWith('/')) { $base += '/' }
    $keys = $SigningKeys -split ' '
    $layout = Get-RbspLayout ([Environment]::GetFolderPath('LocalApplicationData')) ([Environment]::GetFolderPath('Programs'))
    # held until the app is started (Install-RbspApp's last step) and this run's temp folder removed
    $lock = Open-RbspInstallLock $InstallLock 200000
    try {
        $created = -not [IO.Directory]::Exists($layout.Apps)
        [void][IO.Directory]::CreateDirectory($layout.Apps)
        $tmp = [IO.Path]::Combine($layout.Apps, ".bootstrap-$PID")
        [void][IO.Directory]::CreateDirectory($tmp)
        try {
            Write-Host 'Downloading RB Stems Plus...'
            Get-RbspDownload "${base}payload.json" ([IO.Path]::Combine($tmp, 'payload.json'))
            Get-RbspDownload "${base}payload.json.sig" ([IO.Path]::Combine($tmp, 'payload.json.sig'))
            $json = [IO.File]::ReadAllBytes([IO.Path]::Combine($tmp, 'payload.json'))
            $sig = [IO.File]::ReadAllBytes([IO.Path]::Combine($tmp, 'payload.json.sig'))
            if ((Test-RbspPayloadSignature $json $sig $keys) -lt 0) { throw "payload.json's signature doesn't verify" }
            $p = Read-RbspPayload $json
            if ($null -eq $p) { throw $Texts.NotWindows }
            $c = Compare-RbspVersion $p.PayloadVersion $PayloadMinimum
            if ($c -eq 2 -or $c -lt 0) { throw "payload $($p.PayloadVersion) is older than $PayloadMinimum" }
            $zip = [IO.Path]::Combine($tmp, $p.Zip)
            Get-RbspDownload (Get-RbspZipUrl $base $p) $zip
            if ((Get-RbspSha256 $zip) -ne $p.ZipSha256) { throw 'the download is damaged (its SHA-256 differs)' }
            $unpacked = [IO.Path]::Combine($tmp, 'app')
            Test-RbspZip $zip
            Expand-RbspZip $zip $unpacked
            Test-RbspUnpacked $unpacked $p
            $sys = Get-RbspWindowsSys
            Test-RbspAppVersion $unpacked $p $sys
            Install-RbspApp $layout $p $json $sig $unpacked (Get-RbspNativeArch) $sys $keys
            Write-Host "RB Stems Plus $($p.PayloadVersion) is installed."
        } finally {
            Remove-RbspTemp $layout $tmp $created
        }
    } finally {
        Close-RbspInstallLock $lock
    }
}

# This run's temp folder removed; and the app folder too when this run made it and no stub got
# there (a first install that stopped early leaves no empty folder)
function Remove-RbspTemp($layout, [string]$tmp, [bool]$created) {
    if ([IO.Directory]::Exists($tmp)) { try { [IO.Directory]::Delete($tmp, $true) } catch { } }
    if ($created -and -not [IO.File]::Exists($layout.Stub)) { try { [IO.Directory]::Delete($layout.Apps, $true) } catch { } }
}

# ------------------------------------------------------------------ -Restore
# Everything from here to the end runs in ConstrainedLanguage too (R4.I69, J22): cmdlets, property
# reads and casts to core types only; no [ref], no static or instance method call, no New-Object or
# Add-Type, no property set (tests/win/bootstrap checks the text).

# The worker's temporary name for the copy (win/admin/worker.c's NEW): a leftover is removed first
$RestoreTemp = 'onnxruntime.dll.rbsp-new'
# The SIDs trusted to change rekordbox's folder (plat_win.c pw_trusted): Administrators, SYSTEM,
# TrustedInstaller, as SDDL writes them
$TrustedSids = @('BA', 'SY', 'S-1-5-32-544', 'S-1-5-18', 'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')
# SDDL's access-right letters and what they grant (a right not listed counts as a write)
$SddlRights = @{
    GA = [long]0x10000000; GX = [long]0x20000000; GW = [long]0x40000000; GR = [long]2147483648
    SD = [long]0x10000; RC = [long]0x20000; WD = [long]0x40000; WO = [long]0x80000
    FA = [long]0x1F01FF; FR = [long]0x120089; FW = [long]0x120116; FX = [long]0x1200A0
    CC = [long]0x1; DC = [long]0x2; LC = [long]0x4; SW = [long]0x8; RP = [long]0x10; WP = [long]0x20
    DT = [long]0x40; LO = [long]0x80; CR = [long]0x100
}
# The rights that change a folder or its protection (win_rules.h WR_WRITE_RIGHTS)
$WriteRights = [long]0x500D0046

# Whether this window runs with the High mandatory level (an elevated token): S-1-16-12288 in
# whoami /groups (J22; WindowsPrincipal.IsInRole is a .NET call)
function Test-RbspHighToken {
    $g = @()
    try { $g = @(whoami /groups 2>$null) } catch { $g = @() }
    return [bool](@($g | Where-Object { [string]$_ -match 'S-1-16-12288' }).Count)
}

# Program Files, the 64-bit one the worker reads (plat_win.c), a local path: HKLM's
# ProgramW6432Dir first, which 64-bit Windows keeps in both registry views, so an x86
# powershell.exe reads the same folder (BS14); else ProgramFilesDir, except in a 32-bit process on
# 64-bit Windows (PROCESSOR_ARCHITEW6432 set), where that is Program Files (x86)
function Get-RbspProgramFiles {
    $key = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion'
    $pf = ''
    $v = Get-ItemProperty -LiteralPath $key -Name ProgramW6432Dir -ErrorAction SilentlyContinue
    if ($null -ne $v) { $pf = [string]$v.ProgramW6432Dir }
    if (-not $pf -and -not $env:PROCESSOR_ARCHITEW6432) {
        $v = Get-ItemProperty -LiteralPath $key -Name ProgramFilesDir -ErrorAction SilentlyContinue
        if ($null -ne $v) { $pf = [string]$v.ProgramFilesDir }
    }
    if ($pf -notmatch '^[A-Za-z]:\\[^\\]') { throw "couldn't read Windows' Program Files folder" }
    return $pf
}

# Whether $path or a folder above it is a reparse point (a junction or a link), up to the drive's
# root. What isn't there (PathNotFound) is passed over; any other error (access denied) counts as
# a link, so the check fails closed (BS1)
function Test-RbspReparse([string]$path) {
    $p = $path
    while ($p) {
        $i = $null
        try { $i = Get-Item -LiteralPath $p -Force -ErrorAction Stop }
        catch { if (-not ([string]$_.FullyQualifiedErrorId -like 'PathNotFound,*')) { return $true } }
        if ($null -ne $i -and [string]$i.Attributes -match 'ReparsePoint') { return $true }
        $up = Split-Path -Path $p -Parent
        if ($up -eq $p) { break }
        $p = $up
    }
    return $false
}

# An SDDL access mask as a number: hex, or SDDL's two-letter rights; -1 when unread
function Get-RbspSddlMask([string]$r) {
    if ($r -cmatch '^0x[0-9a-fA-F]{1,8}\z') { return [long]$r }
    if ($r -cnotmatch '^(?:[A-Z]{2})+\z') { return [long]-1 }
    $m = [long]0
    foreach ($t in (($r -creplace '([A-Z]{2})', '$1 ') -split ' ')) {
        if (-not $t) { continue }
        if ($SddlRights.Keys -cnotcontains $t) { return [long]-1 }
        $m = $m -bor $SddlRights[$t]
    }
    return $m
}

# Whether an SDDL descriptor lets only administrators change the object (win_rules.h wr_acl_ok):
# the owner trusted; a DACL (not NO_ACCESS_CONTROL); allow and deny ACEs only; every allow ACE
# that isn't inherit-only (IO) and grants a write right is a trusted SID's. Fails closed on what
# it can't read (a conditional ACE, an unknown right).
function Test-RbspSddlAdminOnly([string]$sddl) {
    if ($sddl -cnotmatch '^O:([A-Z0-9-]+?)(?:G:[A-Z0-9-]+?)?D:([A-Z]*)((?:\([^()]*\))*)\z') { return $false }
    $owner = $Matches[1]
    $aces = $Matches[3]
    if ($TrustedSids -cnotcontains $owner) { return $false }
    foreach ($a in ($aces -split '\)')) {
        if (-not $a) { continue }
        $f = @(($a -creplace '^\(', '') -split ';')
        if ($f.Count -ne 6) { return $false }
        if ($f[0] -ceq 'D') { continue }
        if ($f[0] -cne 'A') { return $false }
        if ($f[1] -cmatch '^(?:[A-Z]{2})*?IO') { continue }
        $m = Get-RbspSddlMask $f[2]
        if (($m -lt 0 -or ($m -band $WriteRights)) -and $TrustedSids -cnotcontains $f[5]) { return $false }
    }
    return $true
}

# Whether only administrators can change $dir: its owner and DACL, through Get-Acl's SDDL
function Test-RbspAdminOnly([string]$dir) {
    $sddl = ''
    try { $sddl = [string](Get-Acl -LiteralPath $dir).Sddl } catch { return $false }
    return Test-RbspSddlAdminOnly $sddl
}

function Test-RbspRekordboxOpen { return [bool](@(Get-Process -Name rekordbox -ErrorAction SilentlyContinue).Count) }

function Test-RbspRestoreBusy([string]$ours) {
    foreach ($pr in Get-Process -Name 'Upmgr rekordbox', 'Uninstall rekordbox', $AppName -ErrorAction SilentlyContinue) {
        if ($pr.ProcessName -ne $AppName) { return $true }
        $path = $null
        try { $path = $pr.Path } catch { }
        # A literal, case-insensitive split at the start tests the folder prefix in ConstrainedLanguage.
        if ($path -and ($path -split "$ours\", 2, 'SimpleMatch')[0] -eq '') { return $true }
    }
    return $false
}

# A file's SHA-256 in hex (compared without case)
function Get-RbspFileSha([string]$file) { return [string](Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash }

# Whether $file is AlphaTheta's (compat.c's pin): a valid Authenticode signature whose signer's
# organization is AlphaTheta Corporation and serial number 0104-01-151827. The subject is read
# field by field: a quote or a '+' (a value that could hold ", O=...", a multi-valued name) is
# refused, and O= and SERIALNUMBER= must each appear once, with exactly that value (BS5; win_pin
# reads them by OID)
function Test-RbspAlphaTheta([string]$file) {
    $s = Get-AuthenticodeSignature -LiteralPath $file
    if ($null -eq $s -or [string]$s.Status -cne 'Valid' -or $null -eq $s.SignerCertificate) { return $false }
    $subject = [string]$s.SignerCertificate.Subject
    if ($subject -match '["+]') { return $false }
    $f = @($subject -split ', ')
    $o = @($f | Where-Object { $_ -cmatch '^O=' })
    $n = @($f | Where-Object { $_ -cmatch '^SERIALNUMBER=' })
    return $o.Count -eq 1 -and $o[0] -ceq 'O=AlphaTheta Corporation' -and $n.Count -eq 1 -and $n[0] -ceq 'SERIALNUMBER=0104-01-151827'
}

# The saved set in $set complete (worker.c set_complete): SHA256SUMS is exactly
# "<sha256>  onnxruntime.dll" and a line end, 82 bytes (so no BOM, no other encoding), and
# onnxruntime.dll's SHA-256 is that one. The sha, or '' when it isn't
function Get-RbspSavedSha([string]$set) {
    $sums = Join-Path $set 'SHA256SUMS'
    $lib = Join-Path $set 'onnxruntime.dll'
    if (-not (Test-Path -LiteralPath $sums -PathType Leaf) -or -not (Test-Path -LiteralPath $lib -PathType Leaf)) { return '' }
    if ((Get-Item -LiteralPath $sums -Force).Length -ne 82) { return '' }
    $t = [string](Get-Content -LiteralPath $sums -Raw)
    if ($t -cnotmatch '^([0-9a-f]{64})  onnxruntime\.dll\n\z') { return '' }
    $sha = $Matches[1]
    if ((Get-RbspFileSha $lib) -ne $sha) { return '' }
    return $sha
}

# Pioneer's file of rekordbox $v put back into its folder $dst from the saved set $set (worker.c
# put_back, I49, I71): no reparse point in any of the paths, both folders and the live file the
# administrators' alone. The live file is classified first (worker.c admin_classify): one that
# isn't a file is refused, one that is AlphaTheta's is left. Then the saved copy must be complete
# and AlphaTheta's; it is copied to the temporary name in $dst, verified (admin-only, SHA-256 and
# signature), then moved over the live file (-Force), or, when that is missing (Defender, R4.I71),
# into its place without -Force, which fails if a file appeared meanwhile; verified again. A forced
# move that fails with the live file gone (5.1's -Force deletes it first) is retried once without
# -Force (BS3). Throws the reason.
function Restore-RbspVersion([string]$v, [string]$set, [string]$dst) {
    $saved = Join-Path $set 'onnxruntime.dll'
    $sums = Join-Path $set 'SHA256SUMS'
    $live = Join-Path $dst 'onnxruntime.dll'
    $tmp = Join-Path $dst $RestoreTemp
    if ((Test-RbspReparse $saved) -or (Test-RbspReparse $sums) -or (Test-RbspReparse $live) -or (Test-RbspReparse $tmp)) {
        throw "rekordbox $v's folder or its saved copy is reached through a link"
    }
    if (-not (Test-RbspAdminOnly $dst)) { throw "rekordbox $v's folder can be changed by other accounts" }
    if (-not (Test-RbspAdminOnly $set)) { throw "the folder of rekordbox $v's saved copy can be changed by other accounts" }
    $present = Test-Path -LiteralPath $live
    if ($present) {
        if (-not (Test-Path -LiteralPath $live -PathType Leaf)) { throw ($Texts.RestoreNotFile -f $v) }
        if (-not (Test-RbspAdminOnly $live)) { throw ($Texts.RestoreOthers -f "rekordbox $v's ONNX Runtime") }
        # The bridge is never AlphaTheta-signed (Windows releases are unsigned), so a live file that
        # is AlphaTheta's is Pioneer's: this stands in for the worker's marker check (worker.c:103)
        if (Test-RbspAlphaTheta $live) {
            Write-Host "rekordbox $v has Pioneer's file already."
            return
        }
    }
    $sha = Get-RbspSavedSha $set
    if (-not $sha) { throw "the saved copy of rekordbox $v's file isn't complete" }
    if (-not (Test-RbspAlphaTheta $saved)) { throw "the saved copy of rekordbox $v's file isn't AlphaTheta's" }
    if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force }
    try {
        Copy-Item -LiteralPath $saved -Destination $tmp
        if (-not (Test-RbspAdminOnly $tmp)) { throw ($Texts.RestoreOthers -f "the copy of Pioneer's file in rekordbox $v") }
        if ((Get-RbspFileSha $tmp) -ne $sha -or -not (Test-RbspAlphaTheta $tmp)) { throw "the copy of Pioneer's file in rekordbox $v doesn't verify" }
        try {
            if ($present) { Move-Item -LiteralPath $tmp -Destination $live -Force }
            else { Move-Item -LiteralPath $tmp -Destination $live }
        } catch {
            $why = $_.Exception.Message
            if (-not $present) { throw "couldn't put Pioneer's file into rekordbox $v, where a file may have appeared meanwhile ($why)" }
            if (Test-Path -LiteralPath $live) { throw "couldn't put Pioneer's file into rekordbox $v ($why)" }
            if (Test-Path -LiteralPath $tmp -PathType Leaf) {
                try { Move-Item -LiteralPath $tmp -Destination $live } catch { $why = "$why; $($_.Exception.Message)" }
            }
            if (-not (Test-Path -LiteralPath $live -PathType Leaf)) { throw ($Texts.RestoreGone -f $v, $why) }
        }
    } finally {
        if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue }
    }
    if (-not (Test-Path -LiteralPath $live -PathType Leaf) -or (Get-RbspFileSha $live) -ne $sha -or -not (Test-RbspAlphaTheta $live)) {
        throw "rekordbox $v's file doesn't verify as Pioneer's after putting it back"
    }
    Write-Host "Pioneer's file is back in rekordbox $v."
}

# -Restore (release.md section 4): every saved set <Program Files>\RB Stems Plus\pioneer\<v>\
# whose rekordbox folder <Program Files>\rekordbox\rekordbox <v>\ is there, put back. Elevated
# only; none of our binaries. Refused while rekordbox, its updater or uninstaller, or our worker
# is running. Our folder, pioneer\ and rekordbox\ (the folders the worker holds) must be no link
# and the administrators' alone, when there; never a folder above Program Files, whose default
# ACL isn't (BS1). A version that fails doesn't stop the others (BS7). $pf: Program Files (the
# tests' temp folder), read from the registry when empty.
# Throws the reason, or every version's.
function Invoke-RbspRestore([string]$pf = '') {
    if (-not (Test-RbspHighToken)) { throw $Texts.RestoreElevated }
    if (-not $pf) { $pf = Get-RbspProgramFiles }
    $ours = Join-Path $pf $AppName
    $pioneer = Join-Path $ours 'pioneer'
    $rb = Join-Path $pf 'rekordbox'
    foreach ($d in @($ours, $pioneer, $rb)) {
        if (Test-RbspReparse $d) { throw ($Texts.RestoreLinked -f $d) }
        if ((Test-Path -LiteralPath $d) -and -not (Test-RbspAdminOnly $d)) { throw ($Texts.RestoreOthers -f $d) }
    }
    $todo = @()
    if (Test-Path -LiteralPath $pioneer -PathType Container) {
        foreach ($s in @(Get-ChildItem -LiteralPath $pioneer -Force -Directory)) {
            $v = [string]$s.Name
            if ($v -cnotmatch '^[0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}\z') { continue }
            $dst = Join-Path $rb "rekordbox $v"
            if (Test-Path -LiteralPath $dst) { $todo += ,@($v, (Join-Path $pioneer $v), $dst) }
        }
    }
    if ($todo.Count -eq 0) { throw $Texts.RestoreNone }
    if (Test-RbspRekordboxOpen) { throw $Texts.RestoreOpen }
    if (Test-RbspRestoreBusy $ours) { throw $Texts.RestoreBusy }
    $failed = @()
    foreach ($t in $todo) {
        try { Restore-RbspVersion $t[0] $t[1] $t[2] } catch { $failed += "rekordbox $($t[0]): $($_.Exception.Message)" }
    }
    if ($failed.Count) { throw ($failed -join '; ') }
}

# tests/win/bootstrap dot-sources this block for its functions, with $RbspTestMode set; under iex
# nothing sets it, and StrictMode refuses reading a variable that was never set
if ((Test-Path variable:RbspTestMode) -and $RbspTestMode) { return }
# -Restore is selected by the environment ('1'): irm | iex can't pass a parameter
$restore = $env:RBSTEMSPLUS_RESTORE -eq '1'
# read once: a later install command pasted into the same window installs (BS15)
if (Test-Path -LiteralPath Env:RBSTEMSPLUS_RESTORE) { Remove-Item -LiteralPath Env:RBSTEMSPLUS_RESTORE }
try {
    if ($restore) { Invoke-RbspRestore } else { Invoke-RbspMain }
} catch {
    $m = $_.Exception.Message
    $lead = $Texts.Failed
    if ($restore) { $lead = $Texts.RestoreFailed }
    if ($Texts.Values -contains $m) { Write-Host $m -ForegroundColor Red } else { Write-Host ($lead + $m) -ForegroundColor Red }
}
}
