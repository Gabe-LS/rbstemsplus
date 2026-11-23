# Information since BS2 (bootstrap.ps1 unpacks with its own Expand-RbspZip, whose cases unit-51
# runs on 5.1). Part B4 (app.md 5.4's W13a item): Expand-Archive's own traversal guard on 5.1, on a zip made
# here with a "../" entry (bootstrap.ps1's Test-RbspZip refuses such a name before unpacking;
# this is the guard behind it). Part B5 (information, not a gate): a stored entry whose declared
# size is smaller than its data, through Expand-Archive, with the bytes it writes recorded.
$d = 'C:\rbtest\ea'
if (Test-Path $d) { Remove-Item -Recurse -Force $d }
New-Item -ItemType Directory -Force "$d\out" | Out-Null
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$zip = "$d\traversal.zip"
$a = [IO.Compression.ZipFile]::Open($zip, 'Create')
try {
    $e = $a.CreateEntry('../escape.txt')
    $s = $e.Open(); try { $b = [Text.Encoding]::ASCII.GetBytes('escaped'); $s.Write($b, 0, $b.Length) } finally { $s.Dispose() }
} finally { $a.Dispose() }
$threw = $false
try { Expand-Archive -LiteralPath $zip -DestinationPath "$d\out" -ErrorAction Stop } catch { $threw = $true; "Expand-Archive: $($_.Exception.Message)" }
Check $threw 'a "../" entry: Expand-Archive refuses it'
Check (-not (Test-Path "$d\escape.txt")) 'nothing written outside the destination'
"B5 (information): the size-mismatch zips are the review's fixtures; recorded by hand when they are staged"
Done
