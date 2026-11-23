# kitps-run.ps1: a kit script's runner on the owner's admin login. pc-session.sh copies it with the
# script to %USERPROFILE%\rbsp-kit-admin; its short encoded command checks both files' SHA-256 and
# calls this with the deadline, the prelude (PC check, trace, settings) and the verified script text.
# Nothing comes on stdin (Windows OpenSSH could leave piped stdin undelivered: 2026-11-19).
# Bind the Windows job APIs in memory so the watchdog is armed before any Add-Type compiler,
# script, or native child can block.
param([int]$Seconds, [string]$Prelude, [string]$Script)
if ($Seconds -le 0 -or -not $Script) { 'RBKIT: no deadline or no script'; exit 2 }
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$clock = [Diagnostics.Stopwatch]::StartNew()
function New-RbkitJobKiller {
    $name = [Reflection.AssemblyName]::new('RbkitWatchdog')
    $assembly = [AppDomain]::CurrentDomain.DefineDynamicAssembly($name, 'Run')
    $type = $assembly.DefineDynamicModule($name.Name).DefineType('RbkitJob', 'Public')
    function Bind-JobApi($name, [type]$result, [type[]]$parameters) {
        $method = $type.DefinePInvokeMethod($name, 'kernel32.dll', 'Public, Static, PinvokeImpl',
            'Standard', $result, $parameters, 'Winapi', 'Unicode')
        $method.SetImplementationFlags($method.GetMethodImplementationFlags() -bor 'PreserveSig')
    }
    Bind-JobApi CreateJobObjectW ([IntPtr]) @([IntPtr], [IntPtr])
    Bind-JobApi AssignProcessToJobObject ([bool]) @([IntPtr], [IntPtr])
    Bind-JobApi TerminateJobObject ([bool]) @([IntPtr], [uint32])
    $api = $type.CreateType()
    $job = $api::CreateJobObjectW([IntPtr]::Zero, [IntPtr]::Zero)
    if ($job -eq [IntPtr]::Zero -or -not $api::AssignProcessToJobObject($job, [Diagnostics.Process]::GetCurrentProcess().Handle)) {
        throw 'RB-KIT: could not contain the runner in a Windows job'
    }
    # A managed callback can run even if the PowerShell pipeline is blocked.
    $arg = [Linq.Expressions.Expression]::Parameter([object], 'state')
    $kill = [Linq.Expressions.Expression]::Call($api.GetMethod('TerminateJobObject'),
        [Linq.Expressions.Expression[]]@([Linq.Expressions.Expression]::Constant($job),
            [Linq.Expressions.Expression]::Constant([uint32]142)))
    $body = [Linq.Expressions.Expression]::Block($kill, [Linq.Expressions.Expression]::Empty())
    [Linq.Expressions.Expression]::Lambda([Threading.TimerCallback], $body,
        [Linq.Expressions.ParameterExpression[]]@($arg)).Compile()
}
$rbkitJobKiller = New-RbkitJobKiller
$remaining = [Math]::Max(0, $Seconds * 1000 - $clock.ElapsedMilliseconds)
$global:RbkitWatchdog = [Threading.Timer]::new($rbkitJobKiller, $null, [int]$remaining, -1)
& ([scriptblock]::Create($Prelude + "`n" + $Script))
exit $LASTEXITCODE
