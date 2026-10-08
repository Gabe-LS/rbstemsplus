# Releasing RB Stems Plus

One GitHub Release per version. Users install with one pasted line, which always fetches the
**latest published** release:

```sh
curl -fsSL https://github.com/Gabe-LS/rbstemsplus/releases/latest/download/bootstrap.sh | bash
```

So publishing a release is what ships it. **Releases are built and signed on the developer's
Mac.** GitHub builds nothing that gets signed (there is no release workflow): it only holds the
draft `scripts/release.sh` uploads, and the release once the developer publishes it by hand.

**In short:** commit and push in the development repository → test build of that commit on the
VM (`build.sh --test`) → `scripts/release.sh vX.Y.Z --dry-run` (what would be published) →
`scripts/release.sh vX.Y.Z` (publishes the commit's files as one commit in the public
repository, builds the release there, signs it, tags, uploads a draft) → check the notes and
publish the draft by hand → the real one-liner on a clean VM clone.

## Two repositories

- **Gabe-LS/rbstemsplus-dev** (private): all work happens here, with its whole history, its tags
  and the private documents.
- **Gabe-LS/rbstemsplus** (public): it only receives releases, one commit per release,
  "RB Stems Plus X.Y.Z", author and committer Gabe-LS with its GitHub noreply address. Its
  history holds nothing else. The releases (and so the one-liner) are there.

`.publicignore`, in the development repository, lists the private paths, one per line (a folder
takes everything in it). They are never published, and neither is `.publicignore` itself. A new
private file goes into `.publicignore` in the same commit.

### Once: setting up the two repositories (developer actions)

1. Rename the current GitHub repository to `rbstemsplus-dev`, make it private, and point the
   checkout at it:
   ```sh
   git remote set-url origin https://github.com/Gabe-LS/rbstemsplus-dev.git
   ```
2. Create the empty public repository `Gabe-LS/rbstemsplus`, and turn on Settings › Security ›
   Private vulnerability reporting there. The first release makes its first commit.
3. Clone it next to the development checkout:
   ```sh
   git clone https://github.com/Gabe-LS/rbstemsplus.git "../RB Stems Plus (public)"
   ```
   Another folder works too: pass `--public PATH` to `release.sh`, or set it once with
   `git config rbstemsplus.publicClone PATH`.
4. Set the public identity's email to your GitHub noreply address (GitHub › Settings › Emails):
   ```sh
   git config rbstemsplus.publicEmail <ID>+Gabe-LS@users.noreply.github.com
   ```
   The public commits and tags are made with this identity, never with the global git identity.
5. Protect the tags on the public repository: in Settings › Rules › Rulesets, add a tag ruleset
   for `v*` (Active) with Restrict updates, Restrict deletions and Block force pushes. A moved
   tag would no longer name the commit a release was built from; `release.sh` refuses a tag that
   doesn't point at the release's commit, but the rule keeps it from being moved on GitHub. On a
   free account GitHub enforces rulesets only on public repositories: the rule takes effect when
   the repository goes public, and the private dev repository goes without (nothing is released
   from it).

## What a release contains

`scripts/build.sh` (or `make`) builds all of it into `dist/`; `scripts/release.sh` runs it in the
public clone, at the public commit:

| Asset | What |
|---|---|
| `rbstemsplus-app.zip` | the app, universal, macOS 12+, ad-hoc signed with the hardened runtime (the bootstrap signs it again on the Mac, the same way) |
| `libonnxruntime.1.18.0.dylib` | the Stems Cache bridge, universal, libFLAC linked statically |
| `stemsplus-model.onnx` | the model, 308,572,524 bytes, SHA-256 `0cc50d87…` (never in git) |
| `bootstrap.sh` | the pasted installer (`scripts/bootstrap.sh`, with the public keys and its version written in) |
| `NOTICE`, `LICENSE` | the licences (also inside the app; libFLAC's BSD licence asks for its notice with binaries) |
| `payload.json` | versions, file names, SHA-256s, the compatible rekordbox / ORT / STEMS Engine versions, the public commit it was built from |
| `SHA256SUMS` | `shasum -a 256` of all of the above |
| `payload.json.sig` | the signature of `payload.json`, made by `scripts/release.sh` (never by `build.sh` alone) |

Every file is in every release, because the app and the bootstrap download from
`releases/latest/download/`.

## Signed releases

`payload.json` names every other file by SHA-256, so its signature covers the whole release.
`payload.json.sig` is a DER ECDSA P-256 / SHA-256 signature over the exact bytes of
`payload.json` (`openssl dgst -sha256 -sign`). macOS's own `/usr/bin/openssl` (LibreSSL, on
every macOS from 12) can make and check it, and so can CryptoKit.

- **The app** downloads `payload.json` and `payload.json.sig` together and uses `payload.json`
  only if one of its compiled-in public keys verifies the signature. It keeps the signature next
  to the saved copy and checks it again on every read. It refuses a `payload_version` older than
  the newest it has accepted (`~/Library/Application Support/rbstemsplus/payload-version`), and
  older than its own version (compiled in from `VERSION`), so deleting that record can't bring an
  older release back either. It refuses a `payload.json` in which any object has a key twice
  (JSON readers disagree on which one counts) or a key that isn't plain ASCII. An unsigned saved
  copy (1.0's) is ignored and downloaded again. Uninstalling never needs `payload.json` or the
  network. A refusal is in the app's log; the user sees "Download failed: The download couldn't
  be verified."
- **The bootstrap** downloads both and checks the signature with `/usr/bin/openssl` against the
  same public keys, written into its `SIGNING_KEYS` line by `build.sh`, before it downloads the
  app. It makes the same key check, and refuses a `payload.json` older than its own version
  (its `PAYLOAD_MINIMUM` line, also written by `build.sh`).
- **Two keys**, either one accepted: the **everyday release key**, which signs each release,
  and the **backup key**, kept offline in the password manager for when the everyday key is lost.
  Their public halves are `keys/release.pub.pem` and `keys/backup.pub.pem`, compiled into the app
  by `scripts/build-app.sh` (into `build/app/Keys.swift`).
- **Test builds** (`build.sh --test URL`) trust only a test key, made once in
  `build/test-keys/` (never in git), and their `payload.json` is signed with it. A release build
  stops if `keys/` is missing, if both keys are the same, or if the test key would be in the app
  or the bootstrap.
- **The limit:** the first install trusts the pasted command and GitHub's HTTPS. The bootstrap
  itself is downloaded and run as it is; the signature protects everything it downloads after,
  and every later download by the app.

### Once: making the keys

Run it yourself, in a **new Terminal window** (never through an assistant: the private keys must
not pass through anything else):

```sh
scripts/make-signing-keys.sh
```

0. It first says what follows and waits for Return: the window must be a new one, and Terminal
   must be quit afterwards with **Option-Command-Q** (Quit and Close All Windows). Terminal
   saves its windows' contents to reopen them, so the backup key shown on screen could otherwise
   stay on disk in that saved state.
1. It shows the **backup** private key once. Save it in the password manager as a secure note
   named "RB Stems Plus backup signing key" (every line, BEGIN and END included), press Return;
   the screen and Terminal's scrollback are cleared, and you paste it back once to check the copy
   (not shown). It is never written to disk.
2. It makes the **everyday** key and asks (through openssl) for a passphrase, twice, then once
   more to check it. **Use a long random passphrase** from the password manager (6 or more random
   words, or 20+ random characters) and keep it there: macOS's openssl encrypts the key (PKCS#8,
   AES-256) with a key derived by PBKDF2-SHA1 with only 2048 iterations, so the passphrase is
   what carries the strength. The key is saved at `~/.rbstemsplus-signing/release-key.pem`
   (folder 700, file 600). The passphrase is never on a command line, in the environment or in a
   file.
3. It writes `keys/release.pub.pem` and `keys/backup.pub.pem`. Commit them:
   ```sh
   git add keys && git commit -m "keys: add the release signing keys"
   ```
4. Quit Terminal with Option-Command-Q.

It refuses to overwrite any existing key. Back up `~/.rbstemsplus-signing/release-key.pem` with
the Mac (it is useless without the passphrase).

### If the everyday key is lost (or its passphrase)

Old apps trust only the keys they were built with, so the next release is signed with the
**backup** key, and it brings a new everyday key:

```sh
mv ~/.rbstemsplus-signing/release-key.pem ~/.rbstemsplus-signing/release-key.lost.pem   # if the file is still there
git rm keys/release.pub.pem
scripts/make-signing-keys.sh --release-only        # a new everyday key; keys/backup.pub.pem stays
git add keys && git commit -m "keys: replace the everyday release key"
```

(Again in a new Terminal window, quit afterwards with Option-Command-Q.) Then release as usual,
but with `scripts/release.sh vX.Y.Z --backup-key` (paste the backup key from the password
manager when asked; it isn't shown or saved). Apps that update to that release trust the new
everyday key from then on. If the old everyday key may have been **stolen** rather than lost, say
so in the release notes: apps that haven't updated still trust it until they do.

## The model

The model is not in git. `scripts/build.sh` copies it into `dist/` from
`../Library Organizer/experiments/rekordbox-stems/runs/htdemucs_rb_v4xt.onnx`, or from the path in
`RBSTEMSPLUS_MODEL`, or uses `dist/stemsplus-model.onnx` as it is when that is already right; it
always checks the size and SHA-256 (`model_sha256`, `model_size` in `scripts/build.sh`). If the
local file is lost, download it from any published release (or the prerelease
`model-0cc50d87`, if it still exists) into `dist/stemsplus-model.onnx`: the check is the same.

A new model export means a new hash in `scripts/build.sh` (`model_sha256`, `model_size`),
`ourModel`/`ourModelSize` in `app/Sources/Paths.swift`, and a release note (the cache starts
empty: its folder is named after the hash). The old hash goes into `previous_models` in
`scripts/build.sh` and stays in `builtInModels` (`app/Sources/Models.swift`): the app recognises
its models only by these checksums, and must never save an old one of its own as rekordbox's.

## A new STEMS Engine (rekordbox's own model)

Stems Cache saves the stems of rekordbox's own model only for the models listed in the bridge
(since 1.1). A new STEMS Engine's model stays uncached (its light yellow, "doesn't support STEMS
Engine … yet") until a release lists it:

1. **Its checksum:** the SHA-256 of the new `hdemucs.onnx`, from a Mac that downloaded it.
2. **The null test inside rekordbox** (the test results, "The bridge rebuilds Pioneer's stems
   exactly"): build a test release from the local branch `spike/pioneer-null-test` (its test
   code acts on any model with a spectrogram output, no list needed), install it on a VM with
   the new STEMS Engine, and check that drums, bass and vocals play silence (the -60 and -80
   dBFS beeps only). If they don't, the new model needs a different rebuild: don't list it.
3. **List it** in `bridge/rbstems_bridge.c`: a `PIONEER_xxxx` define, added to `pioneer_models`
   and to the `rbstems_caps` line (the app reads that line from the installed bridge).
   `REBUILD_VERSION` changes only when the rebuild itself changes (it then orphans every rebuilt
   entry); a new listed model doesn't need it.
4. **Allow it for Demucs v4 too** if it should be (`compat_stems_engine` in `scripts/build.sh`),
   which is separate.
5. **Release notes:** users with Stems Cache click Reinstall Stems Cache once to get the new
   list.

## Requirements (on the developer's Mac)

- **Xcode 26 or later.** `actool` compiles the Icon Composer document
  (`app/icon/RBStemsPlus.icon`) only from Xcode 26 on.
- **Network for the first bridge build:** `bridge/build.sh` downloads libFLAC's source tarball
  once into `build/flac/`, checks its SHA-256 on every build, and builds libFLAC from it every
  time (no library built earlier is reused).
- **The signing keys:** the public ones in `keys/` (committed), the everyday private key in
  `~/.rbstemsplus-signing/`, or the backup key in the password manager.
- **gh**, signed in (`gh auth login`, by the developer) with access to both repositories.
- **A clean checkout** of the development repository at a commit pushed to its `origin/main`
  (`release.sh` checks both).
- **The public clone** and `rbstemsplus.publicEmail` (set up once, above). `build.sh` runs there,
  at the public commit: without `--test` it stops on any uncommitted or untracked file and writes
  that commit (`git rev-parse HEAD`) into `payload.json`.

## Checklist

### 1. Prepare

- [ ] Bump `VERSION` (x.y.z). `build-app.sh` writes it into the app's Info.plist and compiles it
      in (the oldest payload the app accepts); `build.sh` writes it into `payload.json`
      (`payload_version` and `app.version`) and into the bootstrap (`PAYLOAD_MINIMUM`).
- [ ] **Only if the app changed:** remember that every new app build needs App Management's
      Allow again on every Mac. Ship app changes rarely (the app is a stable shell).
- [ ] Update the compatibility block at the top of `scripts/build.sh` (`compat_rekordbox`,
      `compat_ort_prefix`, `compat_stems_engine`) for any rekordbox or STEMS Engine version
      tested since the last release. Stems Cache is refused on unlisted rekordbox versions.
- [ ] Update the version in `README.md`/docs if they mention it.
- [ ] Write the release notes in `docs/release-notes/X.Y.Z.md`: what changed, whether the app
      changed (App Management will ask again), supported rekordbox versions. `release.sh` makes
      them the draft's notes, and they are published with the release's files.
- [ ] Commit everything and push to `origin/main` (the development repository).

### 2. Check locally

```sh
make check
make test
```

### 3. Test the TEST build of that commit on the VM

The VM is a Tart clone of a clean macOS 26 template with the **DJ** admin account, rekordbox
signed in, STEMS on, the STEMS Engine downloaded, and updates off. Always start from a **fresh
clone** of the template.

1. **Make a test build of the commit you will release, and serve it from the host.** The test
   build writes the commit into `dist-test/payload.json`, with `-dirty` if anything was
   uncommitted: there must be no `-dirty`. A test build's app downloads its files from the local
   server instead of the GitHub release (fixed when it is compiled; its window title says TEST
   BUILD) and trusts only the test key, which signs `dist-test/payload.json`. The release build
   can't be tested this way before publishing.
   ```sh
   git status                                             # nothing to commit
   scripts/build.sh --test http://192.168.64.1:8765/      # into dist-test/
   grep '"commit"' dist-test/payload.json                 # HEAD, no -dirty
   cd dist-test && python3 -m http.server 8765
   ```
   (or the x2 probe server, `x2/server.py`, which also accepts log uploads).
2. **Paste as DJ in Terminal:**
   ```sh
   curl -fsSL http://192.168.64.1:8765/bootstrap.sh | RBSTEMSPLUS_BASE_URL=http://192.168.64.1:8765/ bash
   ```
   - [ ] no warning from Terminal or Gatekeeper; the app opens; `xattr` shows no quarantine
   - [ ] the Dock and Finder show the icon (Apple's shape on macOS 26)
3. **The app's own downloads** (payload.json, the model, the bridge, Update RB Stems Plus) come
   from the same server in a test build. `scripts/build.sh` without `--test` refuses to package an
   app that says TEST BUILD, so a test build can't be released by mistake.
4. **Flows:**
   - [ ] Install Stems Cache alone (rekordbox's own model): password sheet → "RB Stems Plus was
         prevented from modifying apps" → Allow → turn on, confirm → **Later** → Try Again →
         done; `config.ini` has `rekordbox_model=1`
   - [ ] rekordbox: STEMS on the same track twice (first a miss, then a hit, much faster);
         `~/Library/Logs/rbstemsplus/bridge.log` shows "miss … rebuilt", then the hits, and
         nothing "not cached" on ordinary music
   - [ ] Install Demucs v4: no prompt, no password; Pioneer's model saved in `originals/`;
         the same track again is a new first separation (its own cache folder), then hits
   - [ ] Uninstall Demucs v4: no password; Stems Cache stays green; the track loads from the
         stems saved with rekordbox's model
   - [ ] a second, standard account (`sysadminctl -addUser`, rekordbox's `demucs3_model` folder
         copied in): the light says "installed from another account"; Install Stems Cache asks
         no password and turns it on there
   - [ ] an older rekordbox (Pioneer's pkg, over SSH) → the watcher waits for the installer,
         then asks; Remind Me Later; Reinstall Now → the app reinstalls
   - [ ] rekordbox's own update → "rekordbox was prevented…" (Allow) → the watcher asks →
         Reinstall Now
   - [ ] each uninstall, then **Uninstall RB Stems Plus Completely**
5. **Trace audit after the complete uninstall:**
   - [ ] rekordbox's file-tree hash equals the one before the install; `codesign --verify
         --deep --strict` passes and `spctl -a -vv` says "Notarized Developer ID"
   - [ ] Pioneer's model is back (its SHA-256, `435c9878…` on the template)
   - [ ] no `~/Library/Application Support/rbstemsplus`, `~/Library/Logs/rbstemsplus`,
         `~/Library/Caches/rbstemsplus`, `/Library/Application Support/rbstemsplus`,
         LaunchAgent plist or background-item record (`sfltool dumpbtm`)
6. **Faults worth a quick run:** a tampered download (append a byte to the zip on the server:
   the bootstrap must refuse with "checksum mismatch" and change nothing; append a byte to
   `payload.json`, or remove `payload.json.sig`: the bootstrap must refuse with "couldn't be
   verified", and the app's Install must say "Download failed"), wrong password,
   rekordbox running, rekordbox opened during the password sheet (the app must ask to quit it,
   root must exit 5), and a full disk (the app must say "Low disk space" first).
7. **When the root scripts changed:** `tests/root/vm-root-tests.sh` in a throwaway clone (its
   header says how; about 30 minutes).

### 4. Preview what would be published

```sh
scripts/release.sh vX.Y.Z --dry-run
```

It needs no key, gh or Terminal, and changes nothing (it only fetches the public clone's origin).
It makes the checks on `VERSION`, the public clone and the public copy described below, then
shows the commit it would make in the public clone and the public files it would change (every
file, for the first release). Read the list: nothing private may be in it. With uncommitted
files it only notes them, since it copies HEAD as committed.

### 5. Build, sign and upload the draft

On the developer's Mac, in a Terminal window, in the development repository at the VM-tested
commit:

```sh
scripts/release.sh vX.Y.Z
```

It stops at the first problem, before anything is pushed or uploaded:
- `VERSION` is X.Y.Z, the checkout is clean, the key file is this account's alone; HEAD is on
  `origin/main`, the tag `vX.Y.Z` (here or on origin) points at HEAD if it exists, and
  Gabe-LS/rbstemsplus has no release `vX.Y.Z` yet;
- the public clone: `rbstemsplus.publicEmail` is a Gabe-LS noreply address; the clone's origin is
  Gabe-LS/rbstemsplus (and the development repository's isn't); it is on main, clean, and at its
  `origin/main`, or one commit ahead with this very release (from an earlier run that stopped);
- the public copy is HEAD's files minus `.publicignore`'s paths and `.publicignore` itself. It
  refuses if anything private would be published: a listed path missing from HEAD or still in
  the copy, a file named like a private one, a published file that mentions a private file by
  name, or one that holds the developer's own name, account name, or git name or email. The copy
  must be HEAD's files exactly (paths, modes and contents), minus the private ones;
- it commits the copy in the public clone as "RB Stems Plus X.Y.Z", author and committer Gabe-LS
  with the noreply address, on top of the public `origin/main`, and moves the clone's main to it
  (nothing is pushed yet);
- `scripts/build.sh` builds every asset in the public clone, into its `dist/`, with its own
  checks (the clean tree, the commit in `payload.json`, both public keys and no test key in the
  app and the bootstrap, the hardened runtime, libFLAC's checksum, the model's). The model and
  libFLAC's tarball come from the development repository when the public clone has none;
- `payload.json` names the public commit and X.Y.Z, and `SHA256SUMS` matches;
- openssl asks for the everyday key's passphrase and signs `payload.json` into
  `dist/payload.json.sig`; the signature must verify with the public commit's `keys/*.pub.pem`.

Then it shows both commits and the signing key (and whether `dist-test/` is from the same
development commit) and asks you to type **yes**. Only then does it make the tag (annotated) here
if it is missing and push it to the development repository, make the public tag (annotated, by
Gabe-LS), push the public main and tag together, and create the **draft** release `vX.Y.Z` on
Gabe-LS/rbstemsplus with the nine files and `docs/release-notes/X.Y.Z.md` as its notes. A release
that exists is never replaced (no `--clobber`): to make it again, delete the draft by hand first.
It checks the draft (its files' names and sizes, and that its `payload.json` and
`payload.json.sig` are the ones made here). It never publishes.

If it stops after the public commit (a wrong passphrase, or no **yes**), run it again at the same
commit: it uses that unpushed commit again.

- With the backup key: `scripts/release.sh vX.Y.Z --backup-key` (paste it when asked). Another
  key file: `--key PATH`. Another public clone: `--public PATH`.

### 6. Publish

- [ ] Check the draft's notes on GitHub (without `docs/release-notes/X.Y.Z.md` they are only a
      placeholder: write them there).
- [ ] Publish the draft (not as a prerelease, so it becomes `latest`).
- [ ] On a clean VM clone, paste the real one-liner (no override) and run Install once. This is
      the first run of the release build's own downloads, and of its signature with the release
      keys (the VM tests used a test build and the test key).

## The prototype on the developer's Mac

The prototype (test builds up to 4.11, the shell `install.sh` before them) is installed only on
the developer's Mac. Its bridge loads Pioneer's library from `/Library/Application
Support/StemsPlus/ort/`, and its originals are not where this app looks, so RB Stems Plus 1.0
can't reinstall or uninstall its Stems Cache (the app says "Stems Cache from a test version is in
rekordbox: reinstall rekordbox first."). There is no automatic migration from the prototype.
Move to 1.0 by hand, with rekordbox closed (untested):

1. Reinstall rekordbox from Pioneer's installer (rekordbox.com). That puts Pioneer's signed app,
   and its own ONNX Runtime, back.
2. Put Pioneer's stems model back, if the prototype replaced it: copy the saved
   `hdemucs.onnx` (the prototype kept it in `~/Library/Application Support/rbstemsplus/originals/`
   or under `~/Library/Pioneer/rekordbox-stems-backups/`) into
   `~/Library/Application Support/Pioneer/rekordbox6/models/demucs3_model/`. Without a saved copy,
   move `hdemucs.onnx` and `demucs3_ver.txt` out of that folder and turn STEMS on in rekordbox
   once, so it downloads its engine again. Check: the file is no longer 308,572,524 bytes.
3. Remove the prototype's files:
   ```sh
   sudo rm -rf "/Library/Application Support/StemsPlus"
   ```
   Its saved stems in `~/Library/Caches/rekordbox-stems/<model sha>/` may be kept by moving that
   folder to `~/Library/Caches/rbstemsplus/` (the same model export, `0cc50d87…`, so the saved
   stems stay valid; untested); otherwise
   delete them.
   and the prototype app and its LaunchAgent, if any (`launchctl list | grep -i stems`).
4. Install 1.0 with the one-liner. Its first install saves Pioneer's model and files itself.
