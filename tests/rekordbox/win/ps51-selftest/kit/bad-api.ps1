# ps51-check's self-test (not a kit script): each numbered line must be reported, the rest not
Add-Type -AssemblyName WindowsBase, UIAutomationClient, UIAutomationTypes
$p = [Windows.Point]::new()                                          # 3: 5.1 has no parameterless ::new() for a struct
$c = [Windows.Automation.AutomationElement]::Compare($null, $null)   # 4: the real one is [Automation]::Compare
$m = [Math]::Max(1)                                                  # 5: no overload takes 1 argument
$n = [No.Such.Type]::Thing                                           # 6: no such type
$i = [string]::Length                                                # 7: an instance member
Add-Type -TypeDefinition @'
public static class RbspSelfTestCs6 { public static string Hi(string n) => $"hi {n}"; }
'@
Add-Type -Name Ok -Namespace RbspSelfTest -MemberDefinition 'public static int One() { return 1; }'
$ok = [RbspSelfTest.Ok]::One() + [Windows.Automation.Automation]::Compare.Count
$ok = [IO.Path]::Combine('a', 'b', 'c') + [Windows.Point]::new(1, 2) + [string]::Empty
