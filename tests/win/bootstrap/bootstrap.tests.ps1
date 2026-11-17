# scripts/bootstrap.ps1's tests in pwsh on the Mac (app.md W12, release.md T8): the file's rules
# (ASCII, no BOM, no exit), its texts against the core's, versions, the DER signature and ECDSA
# check with keys made here, plainJSONKeys, payload.json's windows block, the layout and the
# records the updater reads (current, installed-release\, payload\, payload-version), the unpacked
# app's checks on a zip made here, and the install on a temp folder with a fake Windows ($sys).
#
#   pwsh -NoProfile -File tests/win/bootstrap/bootstrap.tests.ps1     (tests/win/bootstrap/run.sh)
#
# It also runs in Windows PowerShell 5.1 on Windows (.NET Framework), the one bootstrap.ps1 runs in:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\win\bootstrap\bootstrap.tests.ps1
# There the keys are exported and the signatures made without .NET 5's APIs, the facts are the
# machine's, and the one-liner isn't run (on a supported Windows it would install from GitHub).
$ErrorActionPreference = 'Stop'
$onWindows = $env:OS -eq 'Windows_NT'
$root = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, '..', '..', '..'))
$script:checks = 0
$script:failures = 0
function Check([bool]$cond, [string]$what) {
    $script:checks++
    if (-not $cond) { $script:failures++; Write-Host "FAIL $what" }
}

# ------------------------------------------------------------------ the file
$path = [IO.Path]::Combine($root, 'scripts', 'bootstrap.ps1')
$bytes = [IO.File]::ReadAllBytes($path)
$bad = @($bytes | Where-Object { ($_ -gt 0x7e) -or ($_ -lt 0x20 -and $_ -ne 0x0a -and $_ -ne 0x0d -and $_ -ne 0x09) })
Check ($bad.Count -eq 0) 'bootstrap.ps1 is pure ASCII'
Check (-not ($bytes.Length -ge 3 -and $bytes[0] -eq 0xef -and $bytes[1] -eq 0xbb -and $bytes[2] -eq 0xbf)) 'no BOM'
$src = [Text.Encoding]::ASCII.GetString($bytes)
Check (-not ($src -match '(?m)^\s*exit\b')) 'never exit (it would close the window under iex)'
$at = $src.IndexOf("`n& {`n")
Check ($at -gt 0) 'the whole script runs inside & { }'
Check ($src.TrimEnd().EndsWith('}')) 'and ends with it'

# the functions, without running the install (the block dot-sourced; it returns before Main)
$RbspTestMode = $true
. ([scriptblock]::Create($src.Substring(0, $at) + "`n. {`n" + $src.Substring($at + 5)))

# ------------------------------------------------------------------ texts
$defs = [IO.File]::ReadAllText([IO.Path]::Combine($root, 'core', 'texts.def'))
function Get-CoreText([string]$id) {
    $m = [regex]::Match($defs, "TEXT\($id,\s*win,\s*`"`",\s*`"((?:[^`"\\]|\\.)*)`"\)")
    if (-not $m.Success) { return $null }
    return $m.Groups[1].Value.Replace('\"', '"')
}
Check ($Texts.SacOn -ceq (Get-CoreText 'T_WIN_SAC_ON')) "the Smart App Control refusal is the core's T_WIN_SAC_ON"
Check ($Texts.OldWindows -ceq (Get-CoreText 'T_WIN_START_OLD_WINDOWS')) "the old-Windows refusal is the core's T_WIN_START_OLD_WINDOWS"
Check ($Publisher -ceq 'Gabe-LS') 'the Apps & features Publisher (owner decided)'
$aumidH = [IO.File]::ReadAllText([IO.Path]::Combine($root, 'win', 'c', 'aumid.h'))
Check ($aumidH -match "#define RBSP_AUMID_UTF8 `"$([regex]::Escape($Aumid))`"") 'the AppUserModelID is win/c/aumid.h''s'

# ------------------------------------------------------------------ versions
foreach ($v in @('1.4.0', '08.09.10', '123456789.1.1')) { Check (Test-RbspReleaseVersion $v) "a release version: $v" }
foreach ($v in @('', '1.4', 'v1.4.0', '1.4.0 ', '1234567890.1.1', '1.4.0-beta.1', "1.4.0`n", "1.$([char]0x0661).0")) {
    Check (-not (Test-RbspReleaseVersion $v)) "not a release version: '$v'"
}
$cmp = @(
    @('1.0.9', '1.0.10', -1), @('1.0.10', '1.0.9', 1), @('1.1.0', '1.1.0', 0), @('08.09.10', '8.9.10', 0),
    @('1.5.0', '1.4.9', 1), @('1.5.0-beta.2', '1.5.0', 2), @('1.5.0', '1.5.0-beta.2', 2), @('x', '1.0.0', 2), @('1.0.0', '', 2))
foreach ($c in $cmp) { Check ((Compare-RbspVersion $c[0] $c[1]) -eq $c[2]) "compare $($c[0]) $($c[1]) = $($c[2])" }

# ------------------------------------------------------------------ the signature
$curve = [System.Security.Cryptography.ECCurve]::CreateFromFriendlyName('nistP256')
$k1 = [System.Security.Cryptography.ECDsa]::Create($curve)
$k2 = [System.Security.Cryptography.ECDsa]::Create($curve)
# a P-256 key's SubjectPublicKeyInfo (DER); .NET Framework has no ExportSubjectPublicKeyInfo
function Spki($key) {
    if ($key.PSObject.Methods['ExportSubjectPublicKeyInfo']) { return ,$key.ExportSubjectPublicKeyInfo() }
    $q = $key.ExportParameters($false).Q
    $head = [byte[]](0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
                     0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04)
    return ,[byte[]]($head + $q.X + $q.Y)
}
$pub1 = [Convert]::ToBase64String((Spki $k1))
$pub2 = [Convert]::ToBase64String((Spki $k2))
# a DER INTEGER of a big-endian unsigned value: leading zeros dropped, one 0 back when the top bit is set
function DerInt([byte[]]$v) {
    $i = 0
    while ($i -lt $v.Length - 1 -and $v[$i] -eq 0) { $i++ }
    $b = [byte[]]$v[$i..($v.Length - 1)]
    if ($b[0] -ge 0x80) { $b = [byte[]](@(0) + $b) }
    return ,[byte[]](@(0x02, $b.Length) + $b)
}
function Sign($key, [byte[]]$data) {
    $fmt = 'System.Security.Cryptography.DSASignatureFormat' -as [type]
    if ($fmt) { return ,$key.SignData($data, [System.Security.Cryptography.HashAlgorithmName]::SHA256, $fmt::Rfc3279DerSequence) }
    $rs = $key.SignData($data, [System.Security.Cryptography.HashAlgorithmName]::SHA256)   # r || s (IEEE P1363)
    $body = [byte[]]((DerInt $rs[0..31]) + (DerInt $rs[32..63]))
    return ,[byte[]](@(0x30, $body.Length) + $body)
}
$data = [Text.Encoding]::UTF8.GetBytes('{"payload_version": "1.5.0"}')
$s1 = Sign $k1 $data
Check ((Test-RbspPayloadSignature $data $s1 @($pub2, $pub1)) -eq 1) 'signed by the second key: 1'
Check ((Test-RbspPayloadSignature $data $s1 @($pub2)) -eq -1) 'another key: -1'
$other = [byte[]]$data.Clone(); $other[3] = 0x41
Check ((Test-RbspPayloadSignature $other $s1 @($pub1)) -eq -1) 'changed bytes: -1'
Check ((Test-RbspPayloadSignature $data ([byte[]]$s1[0..19]) @($pub1)) -eq -1) 'a truncated signature: -1'
Check ((Test-RbspPayloadSignature $data ([byte[]]@()) @($pub1)) -eq -1) 'an empty signature: -1'
Check ((Test-RbspPayloadSignature $data $s1 @('not base64!', 'AAAA', $pub1)) -eq 2) 'keys that are not P-256 SPKIs are skipped'
# the DER rules (sig.c's sig_der_to_rs)
for ($t = 0; $t -lt 20; $t++) {
    $s = Sign $k1 $data
    $rs = ConvertFrom-RbspDerSignature $s
    Check ($null -ne $rs -and $rs.Length -eq 64) "a real DER signature reads ($t)"
}
$r = [byte[]](@(0x01) * 32)
$okDer = [byte[]](@(0x30, 0x44, 0x02, 0x20) + $r + @(0x02, 0x20) + $r)
Check ($null -ne (ConvertFrom-RbspDerSignature $okDer)) 'a hand-made DER reads'
$bads = @(
    [byte[]](@(0x30, 0x45, 0x02, 0x21, 0x00) + $r + @(0x02, 0x20) + $r),       # a non-minimal leading zero
    [byte[]](@(0x30, 0x44, 0x02, 0x20) + @(0x81) + $r[1..31] + @(0x02, 0x20) + $r),   # negative
    [byte[]](@(0x30, 0x44, 0x02, 0x20) + @(0) * 32 + @(0x02, 0x20) + $r),     # zero (and non-minimal)
    [byte[]](@(0x30, 0x45, 0x02, 0x20) + $r + @(0x02, 0x20) + $r + @(0x00)),  # bytes after
    [byte[]](@(0x31, 0x44, 0x02, 0x20) + $r + @(0x02, 0x20) + $r),            # not a sequence
    [byte[]](@(0x30, 0x44, 0x02, 0x20) + $r + @(0x02, 0x21) + $r))            # a length past the end
foreach ($b in $bads) { Check ($null -eq (ConvertFrom-RbspDerSignature $b)) 'a DER refused' }

# ------------------------------------------------------------------ plainJSONKeys and payload.json
function B([string]$s) { return ,[Text.Encoding]::UTF8.GetBytes($s) }
$plain = @(
    @('{}', $true), @('{"a": 1, "b": [1, {"a": 2}]}', $true), @('{"a": "x\"y"}', $true),
    @('{"a": 1, "a": 2}', $false), @('{"a": {"b": 1, "b": 2}}', $false), @(('{"a' + [char]0x5c + 'u0062": 1}'), $false), @('{"": 1}', $false),
    @('[1]', $false), @('{"a": 1} x', $false), @('{"a": 1', $false), @("{`"a`": `"x`ny`"}", $false),
    @(('{"a": ' + ('[' * 31) + (']' * 31) + '}'), $true), @(('{"a": ' + ('[' * 32) + (']' * 32) + '}'), $false))
foreach ($c in $plain) { Check ((Test-RbspPlainJson (B $c[0])) -eq $c[1]) "plain keys: $($c[0].Substring(0, [Math]::Min(40, $c[0].Length)))" }

function New-Payload([string]$pv, [string]$wv, [hashtable]$over = @{}) {
    $h = @{ zip = 'rbstemsplus-windows.zip'; sha256 = 'a' * 64; admin = 'b' * 64; x64 = 'c' * 64; arm64 = 'd' * 64 }
    foreach ($k in $over.Keys) { $h[$k] = $over[$k] }
    $mac = '"app": {"version": "1.4.0", "zip": "a.zip", "sha256": "' + ('e' * 64) + '"}, '
    $win = if (-not $wv) { '' } else {
        ', "windows": {"app": {"version": "' + $wv + '", "zip": "' + $h.zip + '", "sha256": "' + $h.sha256 + '", "admin_sha256": "' + $h.admin +
        '", "stub_sha256": {"x64": "' + $h.x64 + '", "arm64": "' + $h.arm64 + '"}}, "bridge": {"file": "b.dll", "sha256": "' + ('f' * 64) + '"}, ' +
        '"compatible": {"rekordbox": ["7.2.19"], "ort_sha256": ["' + ('8' * 64) + '"], "stems_engine": ["0002"]}}'
    }
    return '{"payload_version": "' + $pv + '", ' + $mac + '"model": {"file": "m.onnx", "sha256": "' + ('9' * 64) + '", "size": 9}' + $win + '}'
}
$p = Read-RbspPayload (B (New-Payload '1.5.0' '1.5.0'))
Check ($p.PayloadVersion -eq '1.5.0' -and $p.AppVersion -eq '1.5.0' -and $p.Zip -eq 'rbstemsplus-windows.zip' -and $p.StubArm64 -eq ('d' * 64)) 'a payload read'
Check ($null -eq (Read-RbspPayload (B (New-Payload '1.6.0' $null)))) 'no windows block: $null'
$p = Read-RbspPayload (B (New-Payload '1.5.1' '1.5.0'))
Check ($p.AppVersion -eq '1.5.0' -and $p.PayloadVersion -eq '1.5.1') 'a carried app'
function Refused([string]$json, [string]$what) {
    $ok = $false
    try { [void](Read-RbspPayload (B $json)) } catch { $ok = $true }
    Check $ok "refused: $what"
}
Refused (New-Payload '1.5.0-beta.2' '1.4.0') 'a suffixed payload_version (no beta channel)'
Refused (New-Payload '1.5.0' '1.5.1') 'an app newer than the payload'
Refused (New-Payload '1.5.0' '1.5.0-beta.1') 'a suffixed windows.app.version'
Refused (New-Payload '1.5.0' '1.5.0' @{ zip = '../x.zip' }) 'a zip name with a path'
Refused (New-Payload '1.5.0' '1.5.0' @{ admin = 'B' * 64 }) 'an uppercase hash'
Refused (New-Payload '1.5.0' '1.5.0' @{ x64 = 'c' * 63 }) 'a short hash'
Refused ((New-Payload '1.5.0' '1.5.0').Replace('"model"', '"payload_version": "9.9.9", "model"')) 'payload_version twice'
Refused ((New-Payload '1.5.0' '1.5.0').Replace('"bridge": {"file": "b.dll", ', '"bridge": {')) 'a bridge with no file'
Refused ((New-Payload '1.5.0' '1.5.0').Replace('"file": "b.dll"', '"file": 7')) 'a bridge file that is not a string'
Refused ((New-Payload '1.5.0' '1.5.0').Replace('"bridge": {', '"bridges": {')) 'no bridge'
Refused ((New-Payload '1.5.0' '1.5.0').Replace('"compatible": {', '"compat": {')) 'no compatible'
Refused ((New-Payload '1.5.0' '1.5.0').Replace('"compatible": {"rekordbox": ["7.2.19"], "ort_sha256": ["' + ('8' * 64) + '"], "stems_engine": ["0002"]}', '"compatible": []')) 'a compatible that is not an object'

# ------------------------------------------------------------------ where the zip comes from
$pz = [pscustomobject]@{ PayloadVersion = '1.5.2'; Zip = 'w.zip' }
Check ((Get-RbspZipUrl $BaseUrl $pz) -ceq 'https://github.com/Gabe-LS/rbstemsplus/releases/download/v1.5.2/w.zip') 'from the latest release: the zip from its own tag'
Check ((Get-RbspZipUrl 'http://127.0.0.1:8080/r/' $pz) -ceq 'http://127.0.0.1:8080/r/w.zip') 'a test server: the zip beside payload.json'
$tagged = 'https://github.com/Gabe-LS/rbstemsplus/releases/download/v1.4.0/'
Check ((Get-RbspZipUrl $tagged $pz) -ceq ($tagged + 'w.zip')) 'a release named by its tag: the zip beside payload.json'

# ------------------------------------------------------------------ the refusals before any download
function Can([hashtable]$over) {
    $f = @{ Sac = 0; Language = 'FullLanguage'; Build = 22631; Net = 533320; UacOff = $false; Admin = $false }
    foreach ($k in $over.Keys) { $f[$k] = $over[$k] }
    try { Test-RbspCanInstall $f; return $null } catch { return $_.Exception.Message }
}
Check ($null -eq (Can @{})) 'a standard user on Windows 11 22H2: installs'
Check ($null -eq (Can @{ UacOff = $true })) 'UAC off with a standard token: installs'
Check ((Can @{ Admin = $true }) -ceq $Texts.Elevated) 'an elevated window: refused'
Check ((Can @{ Admin = $true; UacOff = $true }) -ceq $Texts.UacOff) 'an administrator with UAC off: the UAC reason'
Check ((Can @{ Sac = 1; Build = 0; Admin = $true }) -ceq $Texts.SacOn) 'Smart App Control on: said first'
Check ($null -eq (Can @{ Sac = 2 }))'Smart App Control in evaluation: installs'
Check ((Can @{ Language = 'ConstrainedLanguage'; Build = 0 }) -ceq $Texts.Language) 'ConstrainedLanguage: refused before the build'
Check ((Can @{ Build = 22000 }) -ceq $Texts.OldWindows) 'Windows 11 21H2: refused'
Check ((Can @{ Net = 528040 }) -ceq $Texts.OldNet) '.NET Framework older than 4.8.1: refused'
$facts = Get-RbspFacts
if ($onWindows) {
    Check ($facts.Build -eq [Environment]::OSVersion.Version.Build -and $facts.Net -gt 0) "the facts read on Windows: build $($facts.Build), .NET release $($facts.Net)"
} else {
    Check ($facts.Build -eq 0 -and -not $facts.Admin -and $facts.Net -eq 0) 'the facts read on the Mac: no build, no admin'
}

# what runs before the refusals runs in any language mode (ConstrainedLanguage allows only core
# types): no [ref], no static or instance method call, no New-Object, no Add-Type in it
foreach ($fn in @('Get-RbspSacState', 'Get-RbspFacts', 'Test-RbspCanInstall')) {
    $body = (Get-Command $fn).ScriptBlock.ToString()
    Check (-not ($body -match '\[ref\]|::|New-Object|Add-Type|\.[A-Za-z]+\(')) "$fn uses only what ConstrainedLanguage allows"
}

# ------------------------------------------------------------------ the one-liner (irm | iex), with no test variables
# The child gets its command as -EncodedCommand (Windows PowerShell 5.1 drops the double quotes
# embedded in a native command's argument), with $ErrorActionPreference Continue around the call
# (on 5.1 a line on the child's stderr through 2>&1 would otherwise be a terminating
# NativeCommandError that stops this suite). -OutputFormat Text: an encoded, non-interactive child
# with stderr redirected writes CLIXML, which pwsh 7.6's parent dies deserializing. Off Windows the
# constrained child gets a Get-ItemProperty stub with a Windows registry's values first, so the
# facts' parsing really runs there (with no values, the old [ref] line was never reached).
function Invoke-OneLiner([string]$exe, [bool]$constrained) {
    $cmd = '$ProgressPreference = ''SilentlyContinue''; '
    if ($constrained -and -not $onWindows) {
        $cmd += 'function global:Get-ItemProperty { param([string]$LiteralPath, [string]$Name, $ErrorAction) ' +
                '[pscustomobject]@{ CurrentBuildNumber = ''22631''; Release = 533320; EnableLUA = 1; VerifiedAndReputablePolicyState = 0; ' +
                'PROCESSOR_ARCHITECTURE = ''AMD64'' } }; '
    }
    if ($constrained) { $cmd += '$ExecutionContext.SessionState.LanguageMode = ''ConstrainedLanguage''; ' }
    $cmd += 'Get-Content -Raw -LiteralPath $env:RBSP_BOOT_PATH | iex'
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cmd))
    $savedBase = $env:RBSTEMSPLUS_BASE_URL
    $savedEap = $ErrorActionPreference
    $env:RBSTEMSPLUS_BASE_URL = $null
    $env:RBSP_BOOT_PATH = $path
    $o = ''
    try {
        $ErrorActionPreference = 'Continue'
        $o = (& $exe -NoProfile -NonInteractive -OutputFormat Text -EncodedCommand $enc 2>&1 | Out-String)
    } finally { $ErrorActionPreference = $savedEap; $env:RBSTEMSPLUS_BASE_URL = $savedBase; $env:RBSP_BOOT_PATH = $null }
    return ($o -replace "$([char]27)\[[0-9;?]*[A-Za-z]", '').Trim()
}
if ($onWindows) {
    # in a ConstrainedLanguage child: refused before any download (section 4's order: SAC first,
    # so a machine with Smart App Control on gets that text instead). The FullLanguage one-liner
    # isn't run here: on a supported Windows it would go on towards the download.
    $out = Invoke-OneLiner 'powershell' $true
    Check ($out -ceq $Texts.Language -or $out -ceq $Texts.SacOn) "under iex in ConstrainedLanguage: the language (or SAC) refusal alone (got: $out)"
    Check (-not $out.Contains('cannot be retrieved')) 'under iex in ConstrainedLanguage: no StrictMode error'
} else {
    $out = Invoke-OneLiner 'pwsh' $false
    Check ($out -ceq $Texts.OldWindows) "under iex on the Mac: the old-Windows refusal alone (got: $out)"
    Check (-not $out.Contains('cannot be retrieved')) 'under iex: no StrictMode error'
    # the same in a ConstrainedLanguage child (pwsh lets a session lower its own mode on any OS)
    $out = Invoke-OneLiner 'pwsh' $true
    Check ($out -ceq $Texts.Language) "under iex in ConstrainedLanguage on the Mac: the language refusal alone (got: $out)"
}

# ------------------------------------------------------------------ the install on a temp folder
$tmpRoot = [IO.Path]::Combine([IO.Path]::GetTempPath(), 'rbsp-boot-test')   # one fixed name: a failed run's folder goes at the next
# a run stopped in the link case leaves its link: deleted alone first (non-recursive), as below
$bentLink = [IO.Path]::Combine($tmpRoot, 'bent', 'link')
if ([IO.Directory]::Exists($bentLink)) { [IO.Directory]::Delete($bentLink) }
if ([IO.Directory]::Exists($tmpRoot)) { [IO.Directory]::Delete($tmpRoot, $true) }
[void][IO.Directory]::CreateDirectory($tmpRoot)
function Sha([byte[]]$b) { return ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($b))).Replace('-', '').ToLowerInvariant() }

# an unpacked app (release.md 3.1's files), with known worker and stub bytes
function New-Unpacked([string]$dir, [string]$stubTag) {
    $files = @{}
    foreach ($rel in $Required) { $files[$rel] = "bytes of $rel" }
    $files["stub/x64/$AppExe"] = "stub x64 $stubTag"
    $files["stub/arm64/$AppExe"] = "stub arm64 $stubTag"
    foreach ($rel in $files.Keys) {
        $f = Join-RbspPath $dir $rel
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($f))
        [IO.File]::WriteAllBytes($f, [Text.Encoding]::ASCII.GetBytes($files[$rel]))
    }
}
function New-P([string]$pv, [string]$v, [string]$stubTag) {
    return [pscustomobject]@{
        PayloadVersion = $pv; AppVersion = $v; Zip = 'w.zip'; ZipSha256 = 'a' * 64
        AdminSha256 = Sha ([Text.Encoding]::ASCII.GetBytes("bytes of admin/$AppExe"))
        StubX64 = Sha ([Text.Encoding]::ASCII.GetBytes("stub x64 $stubTag")); StubArm64 = Sha ([Text.Encoding]::ASCII.GetBytes("stub arm64 $stubTag"))
    }
}
$script:calls = New-Object System.Collections.ArrayList
$script:watcherRunning = $false
$script:appRunning = $false
$script:openOnStop = $false      # the app opens while the watchers stop
$script:shortcutThrows = $false
$fakeSys = @{
    AppRunning = { param($apps) return $script:appRunning }
    StopWatchers = {
        param($apps)
        $r = $script:watcherRunning; [void]$script:calls.Add("stop $apps")
        if ($script:openOnStop) { $script:appRunning = $true }
        return $r
    }
    SetRegistry = { param($key, $name, $kind, $data) [void]$script:calls.Add("reg $key|$name|$kind|$data") }
    NewShortcut = {
        param($lnk, $target, $aumid)
        if ($script:shortcutThrows) { throw 'no shortcut' }
        [void]$script:calls.Add("lnk $lnk|$target|$aumid")
    }
    Start = { param($exe, $argv) [void]$script:calls.Add("start $exe|$($argv -join ' ')") }
}
$keys = @($pub1)
$layout = Get-RbspLayout ([IO.Path]::Combine($tmpRoot, 'Local')) ([IO.Path]::Combine($tmpRoot, 'Programs'))
Check ($layout.Apps.EndsWith([IO.Path]::Combine('Local', 'Programs', 'RB Stems Plus')) -and $layout.Support.EndsWith([IO.Path]::Combine('Local', 'rbstemsplus'))) 'the layout'
# an unpacked app in a folder of its own, as in the script's .bootstrap-<pid>\app
function Stage([string]$name, [string]$tag) {
    $d = [IO.Path]::Combine($tmpRoot, $name, 'app')
    New-Unpacked $d $tag
    return $d
}
function Starts { return ,@($script:calls | Where-Object { $_ -like 'start *' }) }

# a first install
$json = B (New-Payload '1.5.0' '1.5.0')
$sig = Sign $k1 $json
$un = Stage 'staging1' 'one'
Test-RbspUnpacked $un (New-P '1.5.0' '1.5.0' 'one')
Install-RbspApp $layout (New-P '1.5.0' '1.5.0' 'one') $json $sig $un 'arm64' $fakeSys $keys
function Text([string]$f) { return [IO.File]::ReadAllText($f) }
function Bytes([string]$f) { return [IO.File]::ReadAllBytes($f) }
function Same([byte[]]$a, [byte[]]$b) { return [Convert]::ToBase64String($a) -ceq [Convert]::ToBase64String($b) }
$v150 = [IO.Path]::Combine($layout.Apps, '1.5.0')
Check ([IO.File]::Exists([IO.Path]::Combine($v150, $AppExe)) -and -not [IO.Directory]::Exists($un)) 'the version folder holds the app, staging moved'
Check ((Text $layout.Stub) -ceq 'stub arm64 one') 'the stub of the arch'
$cur = Bytes $layout.Current
Check ([Text.Encoding]::ASCII.GetString($cur) -ceq "1.5.0`n" -and $cur.Length -eq 6) 'current: the version and a line end, no BOM'
foreach ($d in @($layout.Installed, $layout.Payload)) {
    Check (Same (Bytes ([IO.Path]::Combine($d, 'payload.json'))) $json) "payload.json in $([IO.Path]::GetFileName($d))"
    Check (Same (Bytes ([IO.Path]::Combine($d, 'payload.json.sig'))) $sig) "payload.json.sig in $([IO.Path]::GetFileName($d))"
}
$prev = [IO.Path]::Combine($layout.Installed, 'prev')
Check (-not [IO.Directory]::Exists($prev)) 'a first install: no prev\'
Check ((Text $layout.Floor) -ceq "1.5.0`n") 'the floor'
$seen = @($script:calls)
Check ($seen -contains "lnk $($layout.Shortcut)|$($layout.Stub)|io.github.rbstemsplus.app") 'the shortcut starts the stub, with the AppUserModelID'
Check ($seen -contains "reg $RunKey|$RunValue|String|`"$($layout.Stub)`" --watcher") 'the Run value: the stub with --watcher'
Check ($seen -contains "reg $UninstallKey|Publisher|String|Gabe-LS") 'Publisher Gabe-LS'
Check ($seen -contains "reg $UninstallKey|DisplayVersion|String|1.5.0") 'DisplayVersion: the release'
Check ($seen -contains "reg $UninstallKey|UninstallString|String|`"$($layout.Stub)`" --uninstall") 'UninstallString: the stub with --uninstall'
Check (@($seen | Where-Object { $_ -like "reg $UninstallKey|EstimatedSize|DWord|*" }).Count -eq 1) 'EstimatedSize'
$iRun = [Array]::IndexOf($seen, "reg $RunKey|$RunValue|String|`"$($layout.Stub)`" --watcher")
$iLnk = [Array]::IndexOf($seen, "lnk $($layout.Shortcut)|$($layout.Stub)|io.github.rbstemsplus.app")
$iUn = [Array]::IndexOf($seen, "reg $UninstallKey|NoRepair|DWord|1")
Check ($iRun -ge 0 -and $iRun -lt $iUn -and $iUn -lt $iLnk) 'the order: the Run value, the Apps & features entry, then the shortcut'
$starts = Starts
Check ($starts.Count -eq 2 -and $starts[0] -eq "start $($layout.Stub)|--watcher" -and $starts[1] -eq "start $($layout.Stub)|") 'a first install starts the watcher, then the app, both through the stub'

# an update of the app by the script: a carried release and a new stub (its signature is the
# first's, so this pair doesn't verify)
$script:calls.Clear()
[void][IO.Directory]::CreateDirectory([IO.Path]::Combine($layout.Apps, '.bootstrap-1'))
$un = Stage 'staging2' 'two'
$json16 = B (New-Payload '1.6.0' '1.6.0')
Install-RbspApp $layout (New-P '1.6.0' '1.6.0' 'two') $json16 $sig $un 'x64' $fakeSys $keys
Check (-not [IO.Directory]::Exists($v150) -and [IO.Directory]::Exists([IO.Path]::Combine($layout.Apps, '1.6.0'))) 'the older version folder removed'
Check (-not [IO.Directory]::Exists([IO.Path]::Combine($layout.Apps, '.bootstrap-1'))) 'a leftover of the script removed'
Check ((Text $layout.Stub) -ceq 'stub x64 two' -and (Text $layout.Current) -ceq "1.6.0`n" -and (Text $layout.Floor) -ceq "1.6.0`n") 'stub, current and floor moved on'
Check ((Same (Bytes ([IO.Path]::Combine($prev, 'payload.json'))) $json) -and (Same (Bytes ([IO.Path]::Combine($prev, 'payload.json.sig'))) $sig)) 'the pair installed-release\ held, verified, kept in prev\'
$starts = Starts
Check ($starts.Count -eq 2 -and $starts[0] -eq "start $($layout.Stub)|--watcher" -and $starts[1] -eq "start $($layout.Stub)|") 'no watcher was running: one started all the same, then the app'

# a watcher running: stopped first, started again; a reinstall of the version `current` names
$script:calls.Clear()
$script:watcherRunning = $true
$un = Stage 'staging3' 'two'
Install-RbspApp $layout (New-P '1.6.0' '1.6.0' 'two') $json16 $sig $un 'x64' $fakeSys $keys
$c = @($script:calls)
Check ($c[0] -like 'stop *' -and $c -contains "start $($layout.Stub)|--watcher") 'the running watcher stopped first and started again'
Check ([IO.File]::Exists([IO.Path]::Combine($layout.Apps, '1.6.0', $AppExe)) -and -not [IO.Directory]::Exists($un)) 'a reinstall: the version folder replaced'
Check (Same (Bytes ([IO.Path]::Combine($prev, 'payload.json'))) $json) 'a pair that does not verify is not kept in prev\'
$script:watcherRunning = $false

# refusals: the app open; the app opened while the watcher stopped; a stub that isn't the payload's
$script:calls.Clear()
$script:appRunning = $true
$threw = $null
try { Install-RbspApp $layout (New-P '1.7.0' '1.7.0' 'x') $json $sig $un 'x64' $fakeSys $keys } catch { $threw = $_.Exception.Message }
Check ($threw -eq $Texts.AppOpen -and (Text $layout.Current) -ceq "1.6.0`n" -and $script:calls.Count -eq 0) 'the app open: refused, nothing changed, no watcher stopped'
$script:appRunning = $false
$script:calls.Clear()
$script:openOnStop = $true
$un = Stage 'staging4' 'three'
$threw = $null
try { Install-RbspApp $layout (New-P '1.7.0' '1.7.0' 'three') $json $sig $un 'x64' $fakeSys $keys } catch { $threw = $_.Exception.Message }
$starts = Starts
Check ($threw -eq $Texts.AppOpen -and (Text $layout.Current) -ceq "1.6.0`n" -and [IO.Directory]::Exists($un)) 'the app opened while the watcher stopped: refused before anything moved'
Check ($starts.Count -eq 1 -and $starts[0] -eq "start $($layout.Stub)|--watcher") 'and the watcher started again, not the app'
$script:openOnStop = $false
$script:appRunning = $false
$script:calls.Clear()
$un = Stage 'staging5' 'three'
$threw = $null
try { Install-RbspApp $layout (New-P '1.7.0' '1.7.0' 'other') $json $sig $un 'x64' $fakeSys $keys } catch { $threw = $_.Exception.Message }
Check ($null -ne $threw -and (Text $layout.Current) -ceq "1.6.0`n" -and (Text $layout.Stub) -ceq 'stub x64 two') 'a stub that is not the payload''s: refused before current or the stub moved'
$starts = Starts
Check ($starts.Count -eq 1 -and $starts[0] -eq "start $($layout.Stub)|--watcher") 'and the watcher started again, not the app'

# a reinstall whose move in fails: the old version folder moved back, current unchanged
$v160 = [IO.Path]::Combine($layout.Apps, '1.6.0')
[IO.File]::WriteAllText([IO.Path]::Combine($v160, 'marker'), 'old')
$script:calls.Clear()
$un = [IO.Path]::Combine($tmpRoot, 'staging6', 'app')   # missing: the move in fails
[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($un))
$threw = $null
try { Install-RbspApp $layout (New-P '1.6.0' '1.6.0' 'two') $json16 $sig $un 'x64' $fakeSys $keys } catch { $threw = $_.Exception.Message }
Check ($null -ne $threw -and (Text ([IO.Path]::Combine($v160, 'marker'))) -ceq 'old' -and [IO.File]::Exists([IO.Path]::Combine($v160, $AppExe))) 'the move in failed: the old version folder back in place'
Check ((Text $layout.Current) -ceq "1.6.0`n" -and -not [IO.Directory]::Exists([IO.Path]::Combine($tmpRoot, 'staging6', 'old'))) 'current unchanged, nothing left aside'
Check ((Starts) -contains "start $($layout.Stub)|--watcher") 'and the watcher started again'

# an update that stops late (the shortcut fails): current moved on, the watcher started, not the app
$script:calls.Clear()
$script:shortcutThrows = $true
$un = Stage 'staging7' 'three'
$json17 = B (New-Payload '1.7.0' '1.7.0')
$threw = $null
try { Install-RbspApp $layout (New-P '1.7.0' '1.7.0' 'three') $json17 $sig $un 'x64' $fakeSys $keys } catch { $threw = $_.Exception.Message }
$starts = Starts
Check ($threw -eq 'no shortcut' -and (Text $layout.Current) -ceq "1.7.0`n") 'an update stopped at the shortcut: current names the new version'
Check ($starts.Count -eq 1 -and $starts[0] -eq "start $($layout.Stub)|--watcher") 'an update stopped late: the watcher started, the app not'

# a first install that stops late: the Apps & features entry is there, the watcher started
$layout2 = Get-RbspLayout ([IO.Path]::Combine($tmpRoot, 'Local2')) ([IO.Path]::Combine($tmpRoot, 'Programs2'))
$script:calls.Clear()
$un = Stage 'staging8' 'one'
$threw = $null
try { Install-RbspApp $layout2 (New-P '1.5.0' '1.5.0' 'one') $json $sig $un 'x64' $fakeSys $keys } catch { $threw = $_.Exception.Message }
$seen = @($script:calls)
$starts = Starts
Check ($threw -eq 'no shortcut' -and $seen -contains "reg $UninstallKey|UninstallString|String|`"$($layout2.Stub)`" --uninstall") 'a first install stopped at the shortcut: it can be removed in Settings > Apps'
Check ($starts.Count -eq 1 -and $starts[0] -eq "start $($layout2.Stub)|--watcher") 'a first install stopped late: the watcher started, the app not'
$script:shortcutThrows = $false

# a first install that stops before the stub: no app folder left (Invoke-RbspMain's finally)
$layout3 = Get-RbspLayout ([IO.Path]::Combine($tmpRoot, 'Local3')) ([IO.Path]::Combine($tmpRoot, 'Programs3'))
$created = -not [IO.Directory]::Exists($layout3.Apps)
[void][IO.Directory]::CreateDirectory($layout3.Apps)
$tmp3 = [IO.Path]::Combine($layout3.Apps, '.bootstrap-7')
$un = [IO.Path]::Combine($tmp3, 'app')
New-Unpacked $un 'one'
$script:calls.Clear()
$threw = $null
try { Install-RbspApp $layout3 (New-P '1.5.0' '1.5.0' 'other') $json $sig $un 'x64' $fakeSys $keys } catch { $threw = $_.Exception.Message }
finally { Remove-RbspTemp $layout3 $tmp3 $created }
Check ($created -and $null -ne $threw -and -not [IO.Directory]::Exists($layout3.Apps)) 'a first install stopped before the stub: the app folder removed'
Check ((Starts).Count -eq 0) 'and nothing started (no stub)'
# an existing app folder is kept
[void][IO.Directory]::CreateDirectory($layout3.Apps)
Remove-RbspTemp $layout3 $tmp3 $false
Check ([IO.Directory]::Exists($layout3.Apps)) 'an app folder this run did not make: kept'

# the unpacked app's checks, on a zip made here and unpacked as the script does
$un = [IO.Path]::Combine($tmpRoot, 'zipsrc')
New-Unpacked $un 'z'
$zip = [IO.Path]::Combine($tmpRoot, 'w.zip')
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($un, $zip)
$out = [IO.Path]::Combine($tmpRoot, 'unzipped')
Expand-Archive -LiteralPath $zip -DestinationPath $out
$pz = New-P '1.5.0' '1.5.0' 'z'
$ok = $true
try { Test-RbspUnpacked $out $pz } catch { $ok = $false; Write-Host $_.Exception.Message }
Check $ok 'a zip of the app checks out'
function Unpacked-Refused([scriptblock]$bend, [string]$what, [string]$like = '*') {
    $d = [IO.Path]::Combine($tmpRoot, 'bent')
    if ([IO.Directory]::Exists($bentLink)) { [IO.Directory]::Delete($bentLink) }
    if ([IO.Directory]::Exists($d)) { [IO.Directory]::Delete($d, $true) }
    New-Unpacked $d 'z'
    & $bend $d
    $r = $false
    try { Test-RbspUnpacked $d $pz } catch { $r = $_.Exception.Message -like $like }
    Check $r "unpacked refused: $what"
}
Unpacked-Refused { param($d) [IO.File]::Delete([IO.Path]::Combine($d, 'LICENSE')) } 'LICENSE missing'
Unpacked-Refused { param($d) [IO.File]::WriteAllText([IO.Path]::Combine($d, 'admin', $AppExe), 'another worker') } 'another worker'
Unpacked-Refused { param($d) [IO.File]::WriteAllText([IO.Path]::Combine($d, 'stub', 'arm64', $AppExe), 'another stub') } 'another arm64 stub'
# a link inside (on Windows a junction: a symbolic link needs an elevated or developer-mode account)
$linkType = if ($onWindows) { 'Junction' } else { 'SymbolicLink' }
# to a folder that is not above the link, so the refusal can only be the link check's
$lt = [IO.Path]::Combine($tmpRoot, 'linktarget')
[void][IO.Directory]::CreateDirectory($lt)
Unpacked-Refused { param($d) [void](New-Item -ItemType $linkType -Path ([IO.Path]::Combine($d, 'link')) -Target $lt) } 'a link inside' '*holds a link*'
# the link alone first (non-recursive): .NET Framework's recursive delete can throw on a nested mount point
[IO.Directory]::Delete($bentLink)

# the shortcut's C# compiles (it runs only on Windows)
$compiled = $true
try { Add-Type -TypeDefinition $ShortcutCode -Language CSharp } catch { $compiled = $false; Write-Host $_.Exception.Message }
Check ($compiled -and ('RbspShortcut' -as [type])) 'the shortcut code compiles'

[IO.Directory]::Delete($tmpRoot, $true)
Write-Host "bootstrap.ps1: $script:checks checks, $script:failures failed"
if ($script:failures) { throw 'bootstrap.ps1 tests failed' }
