# G1 item 1: the owner's round

From the unmodified 1.0.0 and 1.3.0 TEST tag builds to the core-linked 1.4.0 TEST build
(mac-core.md §9, G1 item 1).

**Who does what**
- Lane L1 runs every command (`tests/harness/g1-round.sh`). The owner only clicks in the VM's window, signed in as DJ.
- One fresh clone per starting point, about 15-20 minutes each. Do 1.3.0 first, then 1.0.0.

**Passwords:** DJ's own password. The owner types it; it is never given to L1.

**Results:** every step's checks go into `build/harness/g1-FROM/verify.txt`. A step that prints "NOT VERIFIED" stops the round. Write down what the window said at that point.

## For each starting point (FROM = v1.3.0, then v1.0.0)

**L1:** `tests/harness/g1-round.sh stage FROM`.
- Makes a fresh clone and records rekordbox's file tree.
- Installs the old build with its own bootstrap, as DJ. The app opens.

### Checklist 1: the old app installs both parts (owner)
1. On the welcome sheet, click **Continue**.
2. Start the Stems Cache install:
   - **1.3.0:** on the Manage tab, click **Install Stems Cache**, then **Install**.
   - **1.0.0:** click **Install Stems Plus + Stems Cache**, then **Install**.
3. In the password sheet, type DJ's password and click **Continue**.
4. A notification says "RB Stems Plus was prevented from modifying apps".
   - Click it, or open System Settings › Privacy & Security › App Management.
   - Turn **RB Stems Plus** on and confirm with the password.
   - If macOS offers to quit and reopen it, click **Later**.
   - Back in the app, click **Try Again**.
5. **1.3.0 only:** click **Install Demucs v4**, then **Install**. No password is asked; it is a local 300 MB download.
6. Check: both lights are green and the status line is empty. Tell L1 "installed".

**L1:** `tests/harness/g1-round.sh switch FROM`.
- Records the old watcher's registration.
- Serves the new build, quits the app, removes its saved payload.json, and opens the app again.

### Checklist 2: the in-app update (owner)
1. Check: the status line says "RB Stems Plus 1.4.0 is available".
2. In the RB Stems Plus menu, choose **Update RB Stems Plus…** (on 1.0.0: "Update RB Stems Plus"), then click **Update**.
3. If "prevented from modifying apps" appears, allow it as in checklist 1, step 4.
4. Check:
   - The app quits and a new window opens by itself. Its title still says "TEST BUILD".
   - **About RB Stems Plus** says 1.4.0.
   - **From 1.3.0:** no "Stems Cache update" question appears (the bridge is the same).
   - **From 1.0.0:** the new app offers a Stems Cache update, because 1.0.0 installed an older bridge. Click **Update** and type the password. When it shows **Permission needed**, allow RB Stems Plus as in checklist 1, step 4, click **Later** if offered, then **Try Again** in the same window. The light turns green.
5. Tell L1 what you saw.

**L1:** `tests/harness/g1-round.sh verify-update FROM`. It checks that:
- the app is 1.4.0, the core build, and its log says "updated";
- there is no quarantine and the app verifies;
- the watcher's registration record changed to the new helper's code, and "watcher: installed" was logged after the update;
- a WatchPaths trigger runs the new watcher.

### Checklist 3: App Management for the new build, then Install, Reinstall and uninstall (owner)
1. On the Manage tab, click **Reinstall Stems Cache**, then **Reinstall**, and type the password.
   - Check: the app shows **Permission needed**.
   - **From 1.0.0:** App Management was already allowed for the new build during checklist 2's Stems Cache update, so the reinstall just succeeds. Skip step 2; verify-allow reads checklist 2's refusal and Try Again.
   - Tell L1 whether macOS showed its "prevented from modifying apps" notification. L1 records it with `g1-round.sh note FROM "…"`.
2. Allow RB Stems Plus in System Settings › Privacy & Security › App Management, as in checklist 1, step 4.
   - If macOS offers to quit and reopen the app, click **Later**.
   - Then click **Try Again** in the same window. Do not reopen the app.
   - Check: the light is green again.
3. **L1:** `tests/harness/g1-round.sh verify-allow FROM`. It checks the refusal, then the success after Try Again in the same process.
4. Click **Reinstall Demucs v4**, then **Reinstall**. (From 1.0.0 the model is already there: 1.0.0's **Install Stems Plus + Stems Cache** installed it, so this is Reinstall too.)
5. Click **Uninstall Demucs v4**, then **Uninstall**. Then click **Install Demucs v4**, then **Install**.
6. In the RB Stems Plus menu, choose **Uninstall RB Stems Plus…**, click **Uninstall**, type the password if asked, then click **Done**.
   - Check: the app quits and disappears from /Applications.
   - Tell L1.

**L1:** `tests/harness/g1-round.sh verify-uninstall FROM`, then `g1-round.sh done FROM`. It checks that:
- rekordbox is byte for byte as before and has AlphaTheta's signature;
- nothing of RB Stems Plus is left (checked as root);
- no watcher is registered.

## Pass (per starting point)

Every line of `verify.txt` says ok, and every step ends "verified". Then archive verify.txt with the logs.

**G1 item 1 passes** when both starting points pass. The emulated run (`scenarios/g1-update-core.sh`, from mac-A) is regression coverage only.
