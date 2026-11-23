# I82: run the same extracted AnyCPU exe directly (through Explorer) and, on ARM64, from the zip's x64 stub.
# The stub is only the x64 parent in the Prism lap. Neither lap uses the installed app.
Add-Type @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class ZipStart {
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool IsWow64Process2(IntPtr process, out ushort machine, out ushort native);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetProcessInformation(IntPtr process, int infoClass, byte[] info, int size);
    // the process's own machine: ProcessMachineTypeInfo (9) also tells x64 under emulation on
    // ARM64, which IsWow64Process2 reports as not WOW64 (0, so the native machine)
    public static int Machine(IntPtr process) {
        byte[] info = new byte[8];
        if (GetProcessInformation(process, 9, info, info.Length)) return BitConverter.ToUInt16(info, 0);
        ushort machine, native;
        if (!IsWow64Process2(process, out machine, out native)) throw new Win32Exception();
        return machine == 0 ? native : machine;
    }
}
'@

$payload = Get-Served 'payload.json' | ConvertFrom-Json
$zipName = [string]$payload.windows.app.zip
$version = [string]$payload.windows.app.version
$sha = [string]$payload.windows.app.sha256
$valid = $zipName -match '^[A-Za-z0-9_-][A-Za-z0-9._-]*\.zip$' -and
         $version -match '^[0-9]+\.[0-9]+\.[0-9]+$' -and $sha -match '^[0-9a-f]{64}$'
Check $valid 'the served payload names a Windows zip, its version and its SHA-256'
if (-not $valid) { Done }

$work = Join-Path ([IO.Path]::GetTempPath()) ("rbtest-zip-" + [guid]::NewGuid().ToString('N'))
$zip = Join-Path $work $zipName
$folder = Join-Path $work $version
$exe = Join-Path $folder 'RB Stems Plus.exe'
$launcher = Join-Path $work 'RB Stems Plus.exe'
$app = $null
$stub = $null
try {
    New-Item -ItemType Directory -Path $work | Out-Null
    Invoke-WebRequest -UseBasicParsing -Uri ($VmRun.Url + $zipName) -OutFile $zip -TimeoutSec (Get-WaitSecs 90)
    $matchesPayload = (Get-FileHash -LiteralPath $zip).Hash -eq $sha
    Check $matchesPayload 'downloaded zip matches the served payload'
    if (-not $matchesPayload) { Done }
    Expand-Archive -LiteralPath $zip -DestinationPath $folder

    $archs = @(Get-TestArchs)
    foreach ($arch in $archs) {
        $lap = if ($arch -eq $archs[0]) { "native $arch" } else { 'Prism x64' }
        if ($arch -eq $archs[0]) {
            # through the user's Explorer, as a double-click: this step's PowerShell is x64 under
            # emulation on ARM64 (started by hidden.exe), and a start from it runs the AnyCPU exe
            # as x64
            Start-Process -FilePath "$env:WINDIR\explorer.exe" -ArgumentList "`"$exe`""
            [void](Wait-Until {
                $script:app = Get-Process -Name 'RB Stems Plus' -ErrorAction SilentlyContinue |
                    Where-Object { $_.Path -ieq $exe } | Select-Object -First 1
                $null -ne $script:app
            } 30)
        } else {
            Copy-Item -LiteralPath (Join-Path $folder 'stub\x64\RB Stems Plus.exe') -Destination $launcher
            Set-Content -LiteralPath (Join-Path $work 'current') -Value $version -Encoding ASCII
            $stub = Start-Process -FilePath $launcher -PassThru
            $stubOk = $stub.WaitForExit((Get-WaitSecs 15) * 1000) -and $stub.ExitCode -eq 0
            Check $stubOk 'the shipped x64 stub launched the extracted app'
            if (-not $stubOk) { continue }
            [void](Wait-Until {
                $script:app = Get-Process -Name 'RB Stems Plus' -ErrorAction SilentlyContinue |
                    Where-Object { $_.Path -ieq $exe } | Select-Object -First 1
                $null -ne $script:app
            } 30)
        }
        Check ($null -ne $app) "$lap process is running from $exe"
        if (-not $app) { continue }
        $machine = [ZipStart]::Machine($app.Handle)
        $want = if ($arch -eq 'arm64') { 0xAA64 } else { 0x8664 }
        Check ($machine -eq $want) ("$lap process machine 0x{0:X4} (expected 0x{1:X4})" -f $machine, $want)
        $cacheButton = [regex]::Escape((Get-CoreText 'T_BTN_INSTALL_CACHE'))
        $managed = Find-UiText $app.Id "^$cacheButton$" 60
        if (-not $managed) {
            "what $lap shows instead:"
            [void](Invoke-Rbui tree --pid $app.Id --depth 8)
        }
        Check $managed "$lap reached the core-rendered Manage window"
        $core = @(Get-Process -Id $app.Id -Module | Where-Object { $_.ModuleName -ieq 'rbsp-core.dll' })
        $wantCore = Join-Path $folder "$arch\rbsp-core.dll"
        Check ($core.Count -eq 1 -and $core[0].FileName -ieq $wantCore) "$lap loaded exactly $wantCore"
        # a Process object caches MainWindowHandle at its first read (0 when it was found before
        # its window): read it again
        $app.Refresh()
        # a first start (no earlier step answered the welcome in this account) shows the modal
        # welcome, and the main window refuses Close under it: answer it first
        if (Find-UiText $app.Id ('^' + [regex]::Escape((Get-CoreText 'T_WELCOME_TITLE')) + '$') 2) {
            Check (Invoke-UiNamed $app.Id (Get-CoreText 'T_CONTINUE') 30 'Button') "$lap welcome answered: Continue"
        }
        if (-not $app.HasExited) { [void]$app.CloseMainWindow() }
        $closed = $app.WaitForExit((Get-WaitSecs 20) * 1000)
        Check ($closed -and $app.ExitCode -eq 0) "$lap exited normally (closed $closed, exit code $(if ($closed) { $app.ExitCode }))"
        if (-not $closed) { Done }
        $app.Dispose()
        $app = $null
    }
} finally {
    foreach ($p in @($app, $stub)) {
        if ($p) {
            if (-not $p.HasExited) { $p.Kill(); [void]$p.WaitForExit(5000) }
            $p.Dispose()
        }
    }
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
Done
