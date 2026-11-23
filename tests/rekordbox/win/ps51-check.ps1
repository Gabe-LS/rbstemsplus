# ps51-check.ps1: the rekordbox test kit's Windows PowerShell 5.1 gate. The kit runs on the owner's PC
# under powershell.exe 5.1, but the Mac tests it with pwsh 7 and fakes, which accept what 5.1 does not
# (2026-11-21: a conditional inside a hash literal; earlier, fakes copying .NET APIs that don't exist).
# Run under powershell.exe 5.1 on Windows (tests/rekordbox/win/ps51-gate.sh does it on the VM):
#   (a) parses every kit .ps1 with 5.1's parser;
#   (b) compiles every Add-Type C# block (-TypeDefinition, -MemberDefinition) with 5.1's Add-Type
#       (the .NET Framework's C# 5 compiler, compiler warnings as errors as 5.1 does) into a DLL in
#       a scratch folder, and loads every -AssemblyName; nothing compiled is ever called;
#   (c) resolves every static [Type]::Member reference against the real .NET Framework types (and
#       the types (b) compiled): the type must exist, the member must be a public static member,
#       and a call ([T]::M(...), [T]::new(...)) must match an overload's argument count (5.1 has
#       no implicit parameterless constructor for a struct: [Struct]::new() fails there).
# Prints one line per failure (PARSE / ADDTYPE / STATIC) and a summary; exit 0 when nothing failed,
# 1 when something did, 2 when it couldn't run (not 5.1, no kit).
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File ps51-check.ps1 [-Root DIR] [-Work DIR]
# Root: the folder holding kit\ and kitps-run.ps1 (default: this script's folder).
param([string]$Root = $PSScriptRoot, [string]$Work = '')
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version 2

if ($PSVersionTable.PSVersion.Major -ne 5) {
    "ps51-check: needs Windows PowerShell 5.1 (this is $($PSVersionTable.PSVersion))"; exit 2
}
$files = @(Get-ChildItem -LiteralPath (Join-Path $Root 'kit') -Filter *.ps1 -File -ErrorAction SilentlyContinue | Sort-Object Name)
$runner = Join-Path $Root 'kitps-run.ps1'
if (Test-Path -LiteralPath $runner) { $files += Get-Item -LiteralPath $runner }
if ($files.Count -eq 0) { "ps51-check: no kit scripts under $Root"; exit 2 }
if (-not $Work) { $Work = Join-Path $env:TEMP ("rbkit-ps51-" + $PID) }
$null = New-Item -ItemType Directory -Force -Path $Work

$fw = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\NET Framework Setup\NDP\v4\Full' -ErrorAction SilentlyContinue)
$os = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
"ps51-check: PowerShell $($PSVersionTable.PSVersion) ($($PSVersionTable.PSEdition)), CLR $([Environment]::Version), " +
"NET Framework $(if ($fw) { "$($fw.Version) release $($fw.Release)" } else { '?' }), " +
"process $env:PROCESSOR_ARCHITECTURE on $(if ($os) { "$($os.Caption) $($os.Version) $($os.OSArchitecture)" } else { '?' }), " +
"runtime dir $([Runtime.InteropServices.RuntimeEnvironment]::GetRuntimeDirectory())"

$failures = 0
function Fail([string]$line) { $script:failures++; $line }
function Where-Ast([System.Management.Automation.Language.Ast]$ast) {
    "$($ast.Extent.File | Split-Path -Leaf):$($ast.Extent.StartLineNumber)"
}

# (a) parse
$trees = @{}
foreach ($f in $files) {
    $tokens = $null; $errors = $null
    $tree = [System.Management.Automation.Language.Parser]::ParseFile($f.FullName, [ref]$tokens, [ref]$errors)
    foreach ($e in $errors) {
        Fail "PARSE $($f.Name):$($e.Extent.StartLineNumber):$($e.Extent.StartColumnNumber) $($e.ErrorId): $($e.Message)"
    }
    $trees[$f.Name] = $tree
}
$parseFailures = $failures
"ps51-check: (a) parsed $($files.Count) files, $parseFailures errors"

# the literal text of a parameter's argument; $null when it is computed at run time
function Get-Literal($ast) {
    if ($null -eq $ast) { return $null }
    if ($ast -is [System.Management.Automation.Language.StringConstantExpressionAst]) { return , [string[]]@($ast.Value) }
    if ($ast -is [System.Management.Automation.Language.ExpandableStringExpressionAst]) {
        if ($ast.NestedExpressions.Count -eq 0) { return , [string[]]@($ast.Value) } else { return $null }
    }
    if ($ast -is [System.Management.Automation.Language.ArrayLiteralAst]) {
        $out = @()
        foreach ($e in $ast.Elements) { $v = Get-Literal $e; if ($null -eq $v) { return $null }; $out += $v }
        return , [string[]]$out
    }
    if ($ast -is [System.Management.Automation.Language.ParenExpressionAst] -and
        $ast.Pipeline -is [System.Management.Automation.Language.PipelineAst] -and $ast.Pipeline.PipelineElements.Count -eq 1 -and
        $ast.Pipeline.PipelineElements[0] -is [System.Management.Automation.Language.CommandExpressionAst]) {
        return Get-Literal $ast.Pipeline.PipelineElements[0].Expression
    }
    return $null
}

# (b) Add-Type: -AssemblyName loads first (later blocks and (c) need them), then each C# block
$addTypes = @()
foreach ($name in ($trees.Keys | Sort-Object)) {
    $addTypes += @($trees[$name].FindAll({ param($n)
        $n -is [System.Management.Automation.Language.CommandAst] -and $n.GetCommandName() -eq 'Add-Type' }, $true))
}
$assemblies = @{}; $blocks = @(); $kitTypes = @{}
foreach ($cmd in $addTypes) {
    $bound = [System.Management.Automation.Language.StaticParameterBinder]::BindCommand($cmd, $true)
    $p = @{}
    foreach ($k in $bound.BoundParameters.Keys) { $p[$k] = $bound.BoundParameters[$k].Value }
    if ($p.ContainsKey('AssemblyName')) {
        $names = Get-Literal $p['AssemblyName']
        if ($null -eq $names) { "ADDTYPE SKIP $(Where-Ast $cmd) -AssemblyName is computed at run time" }
        else { foreach ($a in $names) { if (-not $assemblies.ContainsKey($a)) { $assemblies[$a] = Where-Ast $cmd } } }
    }
    if ($p.ContainsKey('TypeDefinition') -or $p.ContainsKey('MemberDefinition')) { $blocks += , @($cmd, $p) }
}
foreach ($a in ($assemblies.Keys | Sort-Object)) {
    try { Add-Type -AssemblyName $a -ErrorAction Stop }
    catch { Fail "ADDTYPE $($assemblies[$a]) -AssemblyName ${a}: $($_.Exception.Message)" }
}
$compiled = 0; $i = 0
foreach ($b in $blocks) {
    $cmd = $b[0]; $p = $b[1]; $i++
    $args5 = @{ OutputAssembly = (Join-Path $Work "kit-block-$i.dll"); OutputType = 'Library' }
    $dynamic = @()
    foreach ($k in 'TypeDefinition', 'MemberDefinition', 'Name', 'Namespace', 'UsingNamespace', 'ReferencedAssemblies', 'CompilerOptions', 'Language') {
        if (-not $p.ContainsKey($k)) { continue }
        $v = Get-Literal $p[$k]
        if ($null -eq $v) { $dynamic += "-$k"; continue }
        if ($k -in 'UsingNamespace', 'ReferencedAssemblies', 'CompilerOptions') { $args5[$k] = $v } else { $args5[$k] = $v -join ',' }
    }
    if ($p.ContainsKey('IgnoreWarnings')) { $args5['IgnoreWarnings'] = $true }
    if ($dynamic.Count) { Fail "ADDTYPE $(Where-Ast $cmd) $($dynamic -join ', ') computed at run time: can't compile it here"; continue }
    $what = if ($args5.ContainsKey('TypeDefinition')) { 'TypeDefinition' } else { "MemberDefinition $($args5['Namespace']).$($args5['Name'])" }
    $errs = $null
    try {
        Add-Type @args5 -ErrorAction SilentlyContinue -ErrorVariable errs
        if (-not (Test-Path -LiteralPath $args5.OutputAssembly)) { throw 'no assembly written' }
        $asm = [Reflection.Assembly]::LoadFile($args5.OutputAssembly)
        foreach ($t in $asm.GetTypes()) { if (-not $kitTypes.ContainsKey($t.FullName)) { $kitTypes[$t.FullName] = $t } }
        $compiled++
    } catch {
        # the compiler's lines (file(line) : message, then >>> the source line), one line each
        # (5.1's error variable holds ErrorRecords and bare exceptions; no stack traces kept)
        $msgs = @(@($errs) + $_ | ForEach-Object {
            if ($_ -is [Management.Automation.ErrorRecord]) { $_.Exception.Message } elseif ($_ -is [Exception]) { $_.Message } else { "$_" } })
        $msgs = @($msgs | ForEach-Object { "$_" -split "`r?`n" } | ForEach-Object { $_.Trim() } |
            Where-Object { $_ -and $_ -notlike 'at *' -and $_ -notlike '--- *' } | Select-Object -Unique)
        Fail "ADDTYPE $(Where-Ast $cmd) $what failed: $($msgs -join ' | ')"
    }
}
"ps51-check: (b) loaded $($assemblies.Count) assemblies, compiled $compiled of $($blocks.Count) C# blocks ($($kitTypes.Count) types)"

# (c) static references. Assembly-qualified literals ([X, Windows.Foundation, ContentType = WindowsRuntime])
# load their assembly, as the kit's own `$null = [X, ...]` lines do, so they resolve first.
function Resolve-KitType($typeName) {
    $t = $null
    try { $t = $typeName.GetReflectionType() } catch { }
    if (-not $t) { try { $t = $typeName.FullName -as [type] } catch { } }
    if (-not $t -and $kitTypes.ContainsKey($typeName.FullName)) { $t = $kitTypes[$typeName.FullName] }
    if (-not $t) { foreach ($k in $kitTypes.Keys) { if ($k -eq $typeName.Name -or $k.EndsWith('.' + $typeName.Name)) { $t = $kitTypes[$k]; break } } }
    $t
}
foreach ($name in $trees.Keys) {
    foreach ($te in $trees[$name].FindAll({ param($n) $n -is [System.Management.Automation.Language.TypeExpressionAst] -and $n.TypeName.AssemblyName }, $true)) {
        $null = Resolve-KitType $te.TypeName
    }
}
$flags = [Reflection.BindingFlags]'Public, Static, FlattenHierarchy, IgnoreCase'
function Test-Arity($parameterSets, [int]$n) {
    foreach ($ps in $parameterSets) {
        $all = @($ps.GetParameters()); $max = $all.Count; $min = 0
        for ($j = 0; $j -lt $all.Count; $j++) {
            $isParams = ($j -eq $all.Count - 1) -and $all[$j].IsDefined([ParamArrayAttribute], $false)
            if ($isParams) { $max = [int]::MaxValue } elseif (-not $all[$j].IsOptional) { $min = $j + 1 }
        }
        if ($n -ge $min -and $n -le $max) { return $true }
    }
    $false
}
$refs = 0; $typesSeen = @{}; $dynamicMembers = 0
foreach ($name in ($trees.Keys | Sort-Object)) {
    $found = $trees[$name].FindAll({ param($n)
        $n -is [System.Management.Automation.Language.MemberExpressionAst] -and $n.Static -and
        $n.Expression -is [System.Management.Automation.Language.TypeExpressionAst] }, $true)
    foreach ($m in $found) {
        if ($m.Member -isnot [System.Management.Automation.Language.StringConstantExpressionAst]) { $dynamicMembers++; continue }
        $refs++
        $member = $m.Member.Value
        $tn = $m.Expression.TypeName
        $label = "[$($tn.FullName)]::$member"
        $t = Resolve-KitType $tn
        if (-not $t) { Fail "STATIC $(Where-Ast $m) $label type not found"; continue }
        $typesSeen[$t.FullName] = 1
        $call = $m -is [System.Management.Automation.Language.InvokeMemberExpressionAst]
        $n = 0; if ($call -and $m.Arguments) { $n = $m.Arguments.Count }
        if ($member -eq 'new') {
            $ctors = @($t.GetConstructors())
            if ($ctors.Count -eq 0) { Fail "STATIC $(Where-Ast $m) $label $($t.FullName) has no public constructor"; continue }
            if ($call -and -not (Test-Arity $ctors $n)) {
                Fail "STATIC $(Where-Ast $m) $label($n arguments): no constructor of $($t.FullName) takes $n ($(($ctors | ForEach-Object { $_.GetParameters().Count } | Sort-Object -Unique) -join '/'))"
            }
            continue
        }
        $members = @($t.GetMember($member, $flags))
        if ($members.Count -eq 0) {
            $inst = @($t.GetMember($member, [Reflection.BindingFlags]'Public, Instance, IgnoreCase'))
            $why = if ($inst.Count) { 'is an instance member, not static' } else { 'no such public static member' }
            Fail "STATIC $(Where-Ast $m) $label on $($t.FullName): $why"; continue
        }
        if ($call) {
            $methods = @($members | Where-Object { $_ -is [Reflection.MethodInfo] })
            if ($methods.Count -eq 0) {
                # a static property holding a delegate can be invoked; anything else can't
                $delegate = @($members | Where-Object { ($_ -is [Reflection.PropertyInfo] -and [Delegate].IsAssignableFrom($_.PropertyType)) -or ($_ -is [Reflection.FieldInfo] -and [Delegate].IsAssignableFrom($_.FieldType)) })
                if ($delegate.Count -eq 0) { Fail "STATIC $(Where-Ast $m) $label() on $($t.FullName): not a method ($($members[0].MemberType))" }
            } elseif (-not (Test-Arity $methods $n)) {
                Fail "STATIC $(Where-Ast $m) $label($n arguments) on $($t.FullName): no overload takes $n ($(($methods | ForEach-Object { $_.GetParameters().Count } | Sort-Object -Unique) -join '/'))"
            }
        }
    }
}
"ps51-check: (c) checked $refs static references to $($typesSeen.Count) types ($dynamicMembers with a computed member name not checked)"
Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue
if ($failures) { "ps51-check: FAIL ($failures)"; exit 1 }
'ps51-check: PASS'
exit 0
