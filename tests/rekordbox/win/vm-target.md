# Windows rekordbox kit on a Parallels VM

Set `RBKIT_TARGET=vm` on the Mac to use an existing Parallels Windows 11 26H2 ARM64 VM. The kit uses `vm-run.sh` with Windows PowerShell 5.1 as SYSTEM for admin work and a windowless scheduled task as the VM's console user for kit commands. Run it from an unsandboxed Mac shell. The VM must already be running. Run one kit session at a time, with no VM suite running against the same VM.

- `RBKIT_VM` names the VM (default `rb-win-hand-rekordbox`, the linked clone the owner activated rekordbox on). `vm-run.sh` never drives the owner's "Windows 11".
- `RBKIT_VM_USER` names the Windows user signed in at its console, the one rekordbox is activated for. It defaults to the Mac's `git config --local rbstemsplus.vmUser`. The kit stops before reaching the VM when neither is set.

After every snapshot revert, run `session display` once the account's session is up (`auto dumps --formats-only` runs it after `session up`). It switches Parallels' high resolution off for the VM, sets 1920×1080 in the session and, when the scale is not yet 100 %, restarts Windows once (never with rekordbox running) so the account signs in at 100 %. That is the PC's layout, which the kit's screenshots and OCR regions are measured on.

The steps below were written for `rb-win-template` with a separate `rbtest` account. On `rb-win-hand-rekordbox` the activated user takes `rbtest`'s place: prepare `C:\rbkit-vm` for that user, move any `C:\rbtest` left by older VM suites aside, and install the MIDI mapping file with rekordbox closed.

## One-time VM preparation

1. Start the existing template from **rekordbox 7.2.19 installed + stems model**. Keep Parallels Pause idle and Guest Shared Folders off. Share only the main checkout's `build/win` as `\\Mac\win` and verify this share is available to SYSTEM and `rbtest`. Keep `C:\rbtest` available for the format probe. It must be absent before a batch. If an older VM suite created it, preserve its contents elsewhere after verifying that no probe is installed.
2. Create a standard local `rbtest` account and sign it in at the VM console. Keep its profile and `%TEMP%` on the system volume, with the standard `AppData\Local\Temp` path. From an elevated **Windows PowerShell 5.1** session, prepare the separate launcher output directory.

   ~~~powershell
   New-Item -ItemType Directory -Force C:\rbkit-vm | Out-Null
   icacls.exe C:\rbkit-vm /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' "$env:COMPUTERNAME\rbtest:(OI)(CI)M"
   ~~~

3. The owner opens rekordbox 7.2.19 and activates it by signing in as `rbtest`. Initialize the installed STEMS engine in that profile, retain the stock runtime, turn off cloud sync and automatic uploads, and turn off Auto Analysis. Install the Windows TEST app with its helper for the kit's library checks. If using the TEST server, trust the TEST CA for `rbtest` and serve the same TEST build that is staged. The kit uses the VM runner’s shared-network TEST endpoint over HTTP and HTTPS. Run `tests/win/vm-run.sh server-address` on the Mac to print it. Set `RBKIT_VM_SERVER=HOST:PORT` for a different endpoint.
4. Install the **Arm64 Windows MIDI Services Tools**. Windows MIDI Services is the open-source option. Its loopback transport exposes paired endpoints to WinMM, the API used by the kit. Create a pair named **Loopback A** and **Loopback B** using Microsoft's Loopback Setup, or let the staged kit create the pair. In rekordbox, map Loopback B using the kit's [MIDI mapping instructions](../README.md#setting-up-once). Verify that the mapping can browse and load a track. [Microsoft source and license](https://github.com/microsoft/MIDI) · [ARM64 releases](https://github.com/microsoft/MIDI/releases) · [Loopback transport](https://microsoft.github.io/MIDI/kb/virtual-loopback/) · [WinMM support](https://blogs.windows.com/windowsexperience/2026/02/17/making-music-with-midi-just-got-a-real-boost-in-windows-11/)
5. Use an English rekordbox UI and a Windows profile with English OCR installed. Keep the VM console unlocked, the Mac display awake, and rekordbox maximized in the kit's two-deck layout. Start at 1920×1080 and 100% scaling. Verify the deck 1 title and format playlist header land inside the shared OCR regions before trusting measurements. Collapse playlist folders before a batch.
6. Close rekordbox normally and take a separate post-activation snapshot named **rekordbox 7.2.19 rbkit activated** on this template. Use it as the VM kit baseline. Never clone the activated VM. Deactivate rekordbox before deleting the activated VM or discarding its active installation.

`session up` checks or reconnects the existing `rbtest` console session. If the account is signed out, sign it in at the VM console. `session down` disconnects that session. Reconnect at the console if Windows requires it. Keep the desktop as found after a STOP so the UI and kit logs remain available.

## Run the format route

From the checkout, use the generated formats corpus, the regular Windows corpus and the approved Windows probe matching rekordbox's architecture. Stage the timing helper built for Windows ARM64 or x64 under emulation. For the format-only run below, the helper is already installed and the TEST payload server check is explicitly skipped. Use the same commands with `RBKIT_VM_SERVER` unset and the matching dual-protocol TEST server running to include the network check.

~~~sh
export RBKIT_TARGET=vm
export RBKIT_VM_SERVER=
KIT=tests/rekordbox/win/pc-session.sh
CORPUS="$PWD/build/rekordbox/corpus"
FORMATS=/absolute/path/to/generated/formats
DIST="$PWD/build/win/dist-test"
PROBE="$PWD/build/win/probe-t1/onnxruntime.dll"
HELPER="$PWD/build/helper-win-timing/rbsp-prepare.exe"

"$KIT" session up &&
"$KIT" stage --dist "$DIST" --corpus "$CORPUS" \
  --formats "$FORMATS" --format-tier published --probe "$PROBE" --helper "$HELPER" &&
"$KIT" preflight --rekordbox 7.2.19 &&
"$KIT" run format-import --rekordbox 7.2.19 &&
"$KIT" auto dumps --formats-only --format-batch 1 --reuse-format-import \
  --rekordbox 7.2.19 --probe "$PROBE"
~~~

The batch writes `build/rekordbox/win-vm/formats-batch-*/keycompare.tsv` and disconnects the test desktop on success. For UI debugging, use `"$KIT" session up`, `"$KIT" run start --rekordbox 7.2.19`, and `"$KIT" run format-uidump --rekordbox 7.2.19`. Use `"$KIT" collect` to copy the logs and screenshots. After a submitted import stops, use `"$KIT" run format-import --resume-import --rekordbox 7.2.19` to check it without submitting again. The [shared format route](../README.md#unattended-one-command-nobody-at-the-pc) explains batch numbering and import refusal handling.
