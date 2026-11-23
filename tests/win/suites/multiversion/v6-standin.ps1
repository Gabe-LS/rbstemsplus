# V6, as SYSTEM: a running "rekordbox 7.2.19" without starting rekordbox (VMs never do): the
# folder's rekordbox.exe kept aside as rekordbox.exe.mvreal and a stand-in put in its place, which
# v6 starts (the gate matches the running image's full path to the folder, compat_procs_classify).
# The stand-in shows a window titled "rekordbox" (a windowless one reads as "rekordbox hasn't
# finished closing", run 4): a small WinForms exe compiled here. v6-restore puts the real one back.
$exe = Get-RbExeOf '7.2.19'
$keep = "$exe.mvreal"
Check ((Get-Sha $exe) -eq $MvExeSha['7.2.19']) "7.2.19's rekordbox.exe is the audited one before"
Move-Item -LiteralPath $exe -Destination $keep -Force
$src = Join-Path $MvDir 'rbwin.exe'
Add-Type -OutputAssembly $src -OutputType WindowsApplication -ReferencedAssemblies System.Windows.Forms -TypeDefinition @'
using System; using System.Windows.Forms;
public static class P {
    [STAThread] public static void Main() {
        var f = new Form(); f.Text = "rekordbox"; f.Width = 400; f.Height = 200;
        var t = new Timer(); t.Interval = 300000; t.Tick += (s, e) => f.Close(); t.Start();
        Application.Run(f);
    }
}
'@
Copy-Item -LiteralPath $src -Destination $exe -Force
Check ((Get-Sha $keep) -eq $MvExeSha['7.2.19'] -and (Test-Path -LiteralPath $exe)) "kept aside as $keep; the stand-in in its place"
Done
