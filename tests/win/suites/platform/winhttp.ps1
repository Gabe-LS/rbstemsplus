# app.md 5.4: update_win.c's WinHTTP download (win_get, through winc-update-wintest --get) against
# the run's own HTTPS server (vm-run.sh's /w3/: a made-up file with Range, redirects, a PAC file
# and a CONNECT tunnel), as the user, for each architecture here:
#   - a 302 to https followed: the whole file (200);
#   - a resume from the middle, straight and over the 302: 206 from that byte, the file whole
#     (Range kept over the redirect);
#   - a 302 to http not followed: its own 302 the answer (UPDATE_NET_HTTP), nothing written;
#   - the user's proxy set to the server's PAC file (WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY): the
#     download goes through the PAC's proxy (a CONNECT in the server's log) and comes whole.
# The same against GitHub's own redirect is winhttp-github's.
$N = 3000000
$K = 1234567
# HTTPS needs the TEST build's CA, which the run trusts when its served folder has test-ca.pem
if (-not (Test-Path -LiteralPath "\\Mac\win\job\$($VmRun.Run)\test-ca.cer")) { Skip 'the served folder has no test-ca.pem: the run serves no HTTPS the VM trusts' }
$base = $VmRun.Url -replace '^http://', 'https://'
$hostPort = ([Uri]$VmRun.Url).Authority
$log = Join-Path $VmRun.Results 'http.log'
# the file /w3/blob/N sends: byte i is i % 251
$want = New-Object byte[] $N
$len = [Math]::Min(251, $N)
[Array]::Copy([byte[]](0..250), 0, $want, 0, $len)
while ($len -lt $N) { $c = [Math]::Min($len, $N - $len); [Array]::Copy($want, 0, $want, $len, $c); $len += $c }
$sha = [Security.Cryptography.SHA256]::Create()
$wantSha = [BitConverter]::ToString($sha.ComputeHash($want)) -replace '-', ''
function Get-Sha([string]$f) { if (Test-Path -LiteralPath $f) { return (Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash } return '' }
# winc-update-wintest --get: its outcome line as an object ($null when it printed none)
function Invoke-Get([string]$exe, [string]$url, [long]$from, [string]$out) {
    $rc = Invoke-Exe $exe @('--get', $url, "$from", "$N", $out)
    $m = [regex]::Match([string]$script:ExeOut, '(?m)^get result (-?\d+) status (-?\d+) err (-?\d+) at (-?\d+) total (-?\d+) bytes (-?\d+)')
    if ($rc -ne 0 -or -not $m.Success) { return $null }
    return [pscustomobject]@{ Result = [int]$m.Groups[1].Value; Status = [int]$m.Groups[2].Value; At = [long]$m.Groups[4].Value
        Total = [long]$m.Groups[5].Value; Bytes = [long]$m.Groups[6].Value }
}
function Cut([string]$f, [long]$n) { $s = [IO.File]::Open($f, 'Open', 'ReadWrite'); try { $s.SetLength($n) } finally { $s.Close() } }
function Get-Connects { if (Test-Path -LiteralPath $log) { return @(Get-Content -LiteralPath $log | Where-Object { $_ -match "`"CONNECT $([regex]::Escape($hostPort)) HTTP" }).Count } return 0 }

# the user's proxy configuration (WinINet's per-connection options, which WinHTTP's automatic
# proxy reads): the PAC URL alone, then the saved registry values back
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace RbspW3 {
public static class Proxy {
    [StructLayout(LayoutKind.Sequential)] struct Opt { public int Option; public IntPtr Value; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct OptList {
        public int Size; public IntPtr Connection; public int Count; public int Error; public IntPtr Options; }
    [DllImport("wininet.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool InternetSetOption(IntPtr h, int option, IntPtr buf, int len);
    public static void Changed() { InternetSetOption(IntPtr.Zero, 39, IntPtr.Zero, 0); InternetSetOption(IntPtr.Zero, 37, IntPtr.Zero, 0); }
    // flags PROXY_TYPE_DIRECT | PROXY_TYPE_AUTO_PROXY_URL, INTERNET_PER_CONN_AUTOCONFIG_URL = pac
    public static bool SetPac(string pac) {
        IntPtr url = Marshal.StringToHGlobalUni(pac);
        int osz = Marshal.SizeOf(typeof(Opt));
        IntPtr opts = Marshal.AllocHGlobal(2 * osz);
        try {
            Marshal.StructureToPtr(new Opt { Option = 1, Value = (IntPtr)(1 | 4) }, opts, false);
            Marshal.StructureToPtr(new Opt { Option = 4, Value = url }, opts + osz, false);
            OptList l = new OptList { Size = Marshal.SizeOf(typeof(OptList)), Connection = IntPtr.Zero, Count = 2, Error = 0, Options = opts };
            IntPtr lp = Marshal.AllocHGlobal(l.Size);
            try {
                Marshal.StructureToPtr(l, lp, false);
                bool ok = InternetSetOption(IntPtr.Zero, 75, lp, l.Size);
                Changed();
                return ok;
            } finally { Marshal.FreeHGlobal(lp); }
        } finally { Marshal.FreeHGlobal(opts); Marshal.FreeHGlobal(url); }
    }
}
}
'@
$ieKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings'
$connKey = "$ieKey\Connections"
$saved = @{}
foreach ($kv in @(@($ieKey, 'AutoConfigURL'), @($ieKey, 'ProxyEnable'), @($connKey, 'DefaultConnectionSettings'), @($connKey, 'SavedLegacySettings'))) {
    $v = (Get-ItemProperty -LiteralPath $kv[0] -ErrorAction SilentlyContinue).($kv[1])
    $saved["$($kv[0])|$($kv[1])"] = $v
}
function Restore-Proxy {
    foreach ($e in $saved.GetEnumerator()) {
        $p, $n = $e.Key -split '\|', 2
        if ($null -eq $e.Value) { Remove-ItemProperty -LiteralPath $p -Name $n -ErrorAction SilentlyContinue }
        else { Set-ItemProperty -LiteralPath $p -Name $n -Value $e.Value }
    }
    [RbspW3.Proxy]::Changed()
}

$ran = $false
foreach ($a in Get-TestArchs) {
    $exe = Get-TestExe $a 'winc-update-wintest.exe'
    if (-not $exe) { Missing "vm/$a/winc-update-wintest.exe isn't staged ($a)"; continue }
    $ran = $true
    $out = "C:\rbtest\w3-$a.bin"
    Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
    $g = Invoke-Get $exe "${base}w3/to-https/blob/$N" 0 $out
    Check ($g -and $g.Result -eq 0 -and $g.Status -eq 200 -and $g.At -eq 0 -and $g.Bytes -eq $N -and (Get-Sha $out) -eq $wantSha) "a 302 to https followed ($a): 200, the whole file"
    if (Test-Path -LiteralPath $out) {
        Cut $out $K
        $g = Invoke-Get $exe "${base}w3/blob/$N" $K $out
        Check ($g -and $g.Result -eq 0 -and $g.Status -eq 206 -and $g.At -eq $K -and $g.Total -eq $N -and $g.Bytes -eq ($N - $K) -and (Get-Sha $out) -eq $wantSha) "a resume from byte $K ($a): 206 from there, the file whole"
        Cut $out $K
        $g = Invoke-Get $exe "${base}w3/to-https/blob/$N" $K $out
        Check ($g -and $g.Result -eq 0 -and $g.Status -eq 206 -and $g.At -eq $K -and $g.Total -eq $N -and $g.Bytes -eq ($N - $K) -and (Get-Sha $out) -eq $wantSha) "a resume over the 302 ($a): Range kept, 206 from byte $K, the file whole"
    }
    $out2 = "C:\rbtest\w3-$a-http.bin"
    Remove-Item -LiteralPath $out2 -Force -ErrorAction SilentlyContinue
    $g = Invoke-Get $exe "${base}w3/to-http/blob/$N" 0 $out2
    Check ($g -and $g.Result -eq 2 -and $g.Status -eq 302 -and $g.Bytes -eq 0 -and (-not (Test-Path -LiteralPath $out2) -or (Get-Item -LiteralPath $out2).Length -eq 0)) "a 302 to http not followed ($a): its own 302 the answer (UPDATE_NET_HTTP), nothing written"
    $out3 = "C:\rbtest\w3-$a-pac.bin"
    Remove-Item -LiteralPath $out3 -Force -ErrorAction SilentlyContinue
    $before = Get-Connects
    try {
        Check ([RbspW3.Proxy]::SetPac("$($VmRun.Url)w3/proxy.pac")) "the user's proxy is the PAC file $($VmRun.Url)w3/proxy.pac"
        $g = Invoke-Get $exe "${base}w3/blob/$N" 0 $out3
    } finally { Restore-Proxy }
    $after = Get-Connects
    Check ($g -and $g.Result -eq 0 -and $g.Status -eq 200 -and $g.Bytes -eq $N -and (Get-Sha $out3) -eq $wantSha) "through the automatic proxy ($a): 200, the whole file"
    Check ($after -gt $before) "the download went through the PAC's proxy ($a): CONNECT $hostPort in the server's log ($before before, $after after)"
}
if (-not $ran) { Skip 'no winc-update-wintest.exe staged (win/c/tests/run.sh, then vm-run.sh stage)' }
Done
