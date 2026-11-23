# As the user: the drive letters this session already uses (Parallels maps the Mac's folders and
# volumes into the user's session, which SYSTEM doesn't see), so the sticks get letters free here too
Get-PSDrive -PSProvider FileSystem | ForEach-Object { "INFO used: $($_.Name) $($_.Root) $($_.DisplayRoot)" }
Set-Content -LiteralPath $UserLetters -Value (@(Get-PSDrive -PSProvider FileSystem | ForEach-Object Name) -join ',') -Encoding ASCII
Check (Test-Path -LiteralPath $UserLetters) "the session's letters saved ($UserLetters)"
Done
