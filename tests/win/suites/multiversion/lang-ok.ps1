# The installer's language picker only (NSIS LangDLL: "Please select a language", a combo box, OK
# and Cancel), with English kept, gets OK through UI Automation. Any other installer window (a
# licence page, a wizard) is reported and left alone: exit 5. Runs as the user with the full token
# (the installer is elevated). Exit 0 OK pressed, 2 no picker found.
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$A = [System.Windows.Automation.AutomationElement]
$C = [System.Windows.Automation.TreeScope]
$root = $A::RootElement
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt 60) {
  $wins = $root.FindAll($C::Children, [System.Windows.Automation.Condition]::TrueCondition) |
    Where-Object { $_.Current.Name -eq 'rekordbox 7.2.20 Install' }
  foreach ($w in $wins) {
    $all = $w.FindAll($C::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    $texts = @($all | ForEach-Object { "$($_.Current.ControlType.ProgrammaticName)='$($_.Current.Name)'" })
    "window: $($texts -join '; ')"
    # the picker's controls are panes (NSIS LangDLL): exactly the prompt, English, OK and Cancel
    $set = @($all | ForEach-Object { $_.Current.Name } | Where-Object { $_ } | Sort-Object)
    if (($set -join '|') -eq 'Cancel|English|OK|Please select a language.') {
      'language picker, English selected: OK (IDOK to the dialog)'
      Add-Type -Namespace W -Name U -MemberDefinition '[DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);'
      [W.U]::PostMessage([System.IntPtr]$w.Current.NativeWindowHandle, 0x0111, [System.IntPtr]1, [System.IntPtr]::Zero) | Out-Null
      Start-Sleep 5
      $left = @($root.FindAll($C::Children, [System.Windows.Automation.Condition]::TrueCondition) | Where-Object { $_.Current.Name -match '(?i)rekordbox|setup|install|licen' })
      foreach ($x in $left) {
        $n = @($x.FindAll($C::Descendants, [System.Windows.Automation.Condition]::TrueCondition) | ForEach-Object { $_.Current.Name } | Where-Object { $_ })
        "after OK, a window: $($n -join '; ')"
        'not touched; the installer is ended'
        Get-Process | Where-Object { $_.Name -like 'Install_rekordbox*' } | Stop-Process -Force
        exit 5
      }
      'the picker closed; no other installer window'
      exit 0
    }
    'an installer window that is not the language picker: not touched; the installer is ended'
    Get-Process | Where-Object { $_.Name -like 'Install_rekordbox*' } | Stop-Process -Force
    exit 5
  }
  Start-Sleep 2
}
'no language picker within 60 s'
exit 2
