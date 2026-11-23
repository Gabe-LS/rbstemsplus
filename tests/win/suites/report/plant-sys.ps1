# As SYSTEM, before Create Report (N4, review NS2): the account's full name set (net user
# /fullname, the M4 row's "Dana Jones"); an Application log error about rekordbox.exe (source
# "Application Error", event 1000, the last 30 minutes' kind Create Report takes) and a WER
# Report.wer of a rekordbox crash in the user's ReportArchive (the last 7 days, with a memory.hdmp
# and an x.dmp beside it that must stay out), each holding the
# marker, the profile folder, its 8.3 alias, the account and the full name: what the zip must keep
# (the marker) and mask (the names).
# the clock on today's time line, and kept there: a reverted snapshot starts on its own date;
# Parallels' time sync later moves the guest to the host's time, and Windows' time service can move
# it back; a jump between planting and Create Report puts the planted event and Report.wer outside
# their windows (30 minutes, 7 days). Windows' time service is stopped for this reverted VM, then
# the clock is set from the run's server (the Mac's time, which Parallels' sync also keeps)
Stop-Service -Name w32time -Force -ErrorAction SilentlyContinue
Set-Service -Name w32time -StartupType Disabled -ErrorAction SilentlyContinue
Check ((Get-Service -Name w32time).Status -eq 'Stopped') "Windows' time service is stopped"
try {
    $resp = Invoke-WebRequest -UseBasicParsing -Uri $VmRun.Url -TimeoutSec 20
    $host_ = [DateTime]::Parse([string]$resp.Headers['Date'], [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::AdjustToUniversal)
    "the VM's clock: $((Get-Date).ToUniversalTime().ToString('u')), the server's: $($host_.ToString('u'))"
    if ([Math]::Abs(((Get-Date).ToUniversalTime() - $host_).TotalSeconds) -gt 60) { Set-Date -Date $host_.ToLocalTime() | Out-Null }
    Check ([Math]::Abs(((Get-Date).ToUniversalTime() - $host_).TotalSeconds) -lt 120) "the VM's clock agrees with the run's server ($((Get-Date).ToString('o')))"
} catch { Check $false "the VM's clock couldn't be set from the run's server: $($_.Exception.Message)" }
$name = (Get-CimInstance Win32_ComputerSystem).UserName
if (-not $name) { Skip 'no user signed in' }
$user = ($name -split '\\')[-1]
$local = Get-LocalUser -Name $user -ErrorAction SilentlyContinue
if (-not $local) { Skip "the signed-in account ($name) isn't a local account" }
$ErrorActionPreference = 'Continue'   # (a native command's stderr is no error here)
$r = & net.exe user $user "/fullname:$RpFullName" 2>&1 | Out-String
$ErrorActionPreference = 'Stop'
"net user $user /fullname:...: $($r.Trim())"
$full = (Get-LocalUser -Name $user).FullName
Check ($full -eq $RpFullName) "the account's full name is '$full'"
$names = Get-RpNames
"names to mask: account '$($names.User)', profile '$($names.Profile)', 8.3 alias '$($names.Short)', full name '$($names.Full)'"
Check ([bool]$names.Short) "the profile folder has an 8.3 alias to mask ($($names.Short))"
$says = "$RpMark rekordbox.exe crashed for $($names.Full) ($($names.User)) at $($names.Profile)\Music\track.mp3 $(if ($names.Short) { "and $($names.Short)\Music" })"
# the event, as WER's own Application Error events carry it: the image name in its EventData
$msg = "Faulting application name: rekordbox.exe, version: 7.2.19.0`r`nFaulting module name: onnxruntime.dll`r`nFaulting application path: $($names.Profile)\AppData\Local\rbtest\rekordbox.exe`r`n$says"
[Diagnostics.EventLog]::WriteEntry('Application Error', $msg, [Diagnostics.EventLogEntryType]::Error, 1000)
$ev = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = 'Application Error'; StartTime = (Get-Date).AddMinutes(-2) } -ErrorAction SilentlyContinue |
    Where-Object { $_.ToXml() -like "*$RpMark*" } | Select-Object -First 1
Check ($null -ne $ev) 'the Application log holds the planted rekordbox.exe error'
# the WER report (UTF-16LE with its BOM, as WER writes it)
$wer = Join-Path $names.Profile ('AppData\Local\Microsoft\Windows\WER\ReportArchive\AppCrash_rekordbox.exe_' + ('ab' * 20) + '_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $wer | Out-Null
$text = "Version=1`r`nEventType=APPCRASH`r`nSig[0].Name=Application Name`r`nSig[0].Value=rekordbox.exe`r`nAppPath=$($names.Profile)\AppData\Local\rbtest\rekordbox.exe`r`nUser=$($names.Full)`r`n$says`r`n"
[IO.File]::WriteAllText((Join-Path $wer 'Report.wer'), $text, [Text.Encoding]::Unicode)
Check (Test-Path -LiteralPath (Join-Path $wer 'Report.wer')) "Report.wer planted ($wer)"
# VRR7: crash dumps beside it with their own marker, which Create Report never takes.
foreach ($d in 'memory.hdmp', 'x.dmp') { [IO.File]::WriteAllText((Join-Path $wer $d), "$RpDumpMark dump $($names.Profile)") }
Check ((Test-Path -LiteralPath (Join-Path $wer 'memory.hdmp')) -and (Test-Path -LiteralPath (Join-Path $wer 'x.dmp'))) 'memory.hdmp and x.dmp planted beside it'
Done
