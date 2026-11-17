# Releasing RB Stems Plus

One GitHub Release per version. Users install with one pasted line, which always fetches the
**latest published** release:

```sh
curl -fsSL https://github.com/Gabe-LS/rbstemsplus/releases/latest/download/bootstrap.sh | bash
```

So publishing a release is what ships it. **Releases are built and signed on the developer's
Mac.** GitHub builds nothing that gets signed (there is no release workflow): it only holds the
draft `scripts/release.sh` uploads, and the release once the developer publishes it by hand.

**In short:** commit and push in the development repository → `scripts/release.sh vX.Y.Z --plan`
(which Mac files this release carries forward unchanged) → test build of that commit in the
plan's shape on the VM (`build.sh --test URL --plan build/gates/<commit>/plan`) → the gates on
that TEST build (the G3 tart run, the performance session) →
`scripts/release.sh vX.Y.Z --dry-run` (what would be published) →
`scripts/release.sh vX.Y.Z` (publishes the commit's files as one commit in the public
repository, builds the release there, signs it, tags, uploads a draft) → check the notes and
publish the draft by hand → the post-publish check (the real update and one-liner, in tart).

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
| `rbstemsplus-app.zip` | the app, universal, macOS 12+ (tested on macOS 26 on Apple silicon only; older versions and Intel Macs should work, untested), ad-hoc signed with the hardened runtime (the bootstrap signs it again on the Mac, the same way) |
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
  stops if `keys/` is missing or if both keys are the same. When run in this checkout, `build.sh`
  also stops if the test key would be in the app or the bootstrap; a release is covered by
  `release.sh`'s own check.
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

## One version for both platforms (from 1.4.0)

A release's `VERSION`, tag and `payload_version` are one number, `x.y.z`. `app.version` is the
version of the last release that changed the Mac app's files, so it can be lower than
`payload_version`. This is the contract between `scripts/release.sh` (which writes
`payload.json`) and every Mac app since 1.0.0 (`Update.swift`, `Payload.swift`):

- **`payload_version` = `VERSION`**, `x.y.z` digits only.
- **`app.version` ≤ `payload_version`**, `x.y.z`, and equal to the `CFBundleShortVersionString`
  inside the zip that `app.zip` names: every app since 1.0.0 refuses to swap in a zip whose
  version isn't `app.version`.
- **One `app.version`, one zip.** A release that leaves the Mac app's files unchanged carries the
  previous release's zip forward byte for byte, with its `app.version` and `app.sha256`. Then no
  app at that version offers an update (the notice needs `app.version` newer than the running
  app), and older apps update once, to that zip. A zip with other bytes always gets a new
  `app.version` (= `VERSION`): the app shows a release's version on the strength of "same
  version, same bytes".
- **The bridge likewise:** a carried bridge keeps its file and `bridge.sha256`. A new hash asks
  every Stems Cache user to update it (with their password).
- **Every release keeps `app` and `bridge`.** Every Mac app since 1.0.0 refuses a `payload.json`
  without them.
- **No beta channel or pre-releases** (owner decided 2026-11-12): every release goes to all users
  through `releases/latest`. A suffixed version (`1.5.0-beta.1`) must never be published:
  `build.sh`, `build-app.sh` and `release.sh` refuse a `VERSION` or tag that isn't `x.y.z`. The
  app and `bootstrap.sh` read only `releases/latest`, which never names a pre-release. Why it
  matters: apps before 1.4.0 accept a `-` in a version and rank `1.5.0-beta.1` above `1.5.0`, so
  such a `payload.json` would raise their floor above the real 1.5.0 (checked against 1.0.0's and
  1.3.0's own code). From 1.4.0 the app also refuses a `payload_version` or `app.version` that
  isn't `x.y.z`, before its floor moves; `bootstrap.sh` already refuses anything but digits and
  dots.
- **New keys only.** Keys may be added (`app.commit`, `bridge.commit`, a block for another
  platform): every app ignores keys it doesn't read. A key never changes meaning, is never given
  twice in one object, and is plain ASCII; nesting stays under 32 levels.

`release.sh --plan` decides, per Mac file, whether this release carries it forward (`--mac=auto`,
the default: the same files, or no Mac input changed since the commit that built it; `keep`;
`new`); the release and its TEST build follow that plan.

**When the Mac app counts as unchanged (`--mac=auto`).** The Mac app isn't byte-reproducible
(two clean builds of one commit differ in `Assets.car`'s actool-written fields, and so in the
executable's signature hashes and `CodeResources`' seal). So `release.sh` builds the app again
from the public copy, stamped with the previous `app.version`, and compares it with the previous
release's zip (`scripts/bundle-compare.sh`):
- **the same files**, or **the same code** (only those rebuild fields differ, and the asset
  catalog's inputs, `app/icon/`, are unchanged): the previous zip is carried, byte for byte;
- different, but **no Mac input changed** since the commit that built it (`app.commit`, else the
  previous payload's top-level commit): a toolchain's drift only, carried too, and the plan says
  why;
- **a Mac input changed**: a new app, `app.version` = `VERSION`. Every Mac user gets the update
  notice, and App Management asks again on every Mac.

The bridge is reproducible by design: it is carried when it builds the same bytes. `--mac=keep`
carries both even though the build differs (a Windows release where shared code moved but
nothing changed for Mac users); `--mac=new` always makes a new app. After a release from a
branch, `auto` is refused on main until the owner chooses `keep` or `new`. Windows lanes keep the
Mac bytes still: `make mac-unchanged` is the merge check for every Windows-lane commit that
touches `core/`, `common/`, `helper/` or `bridge/`.

**A new Mac app with the previous release's bridge (the G2 fallback).** When the new bridge may
not ship (its performance gate failed, say) but the app is new, restore the bridge's sources to
the release whose bridge is current, so they build its bytes again; the bridge build is
reproducible (v1.3.0's rebuilds as `cf1ebef1…`, its archived TEST build's), and the plan then
carries it:
```sh
git checkout vA.B.C -- bridge/            # also common/ once the bridge builds from it
git commit -m "bridge: back to vA.B.C's sources, so its bytes are carried"
scripts/release.sh vX.Y.Z --plan --mac=new # must say: mac.bridge carry <its commit> <its sha>
```
If the plan says `mac.bridge new`, the rebuild isn't byte-identical (a toolchain change): stop
there, that release would ask every Stems Cache user for an update and their password. The
helper reads `bridge/build.sh`'s libFLAC pin and the bridge's headers too, so the new app's
helper is built from the restored ones.

What the app shows: About, the report and the logs show the release's version, the
`payload_version` of the last verified `payload.json` whose `app.version` is this very app
("RB Stems Plus 1.4.1 (Mac app 1.4.0, …)" in the report and logs). It is recorded in
`~/Library/Application Support/rbstemsplus/shown-version` with the app's own version, kept when a
newer app is announced but not installed yet, and dropped when the app is replaced. Finder's Get
Info shows the app's own version. Apps before 1.4.0 show their own version.

## The gates (from 1.4.0)

Every gate runs on the TEST build of the release commit in the plan's shape (step 3: `release.sh
--plan`, then `build.sh --test URL --plan`), so it tests what the release will carry. The records
live in `build/gates/<commit>/`, next to `plan`.

- **Static gates, every build** (`scripts/build-app.sh`, so the release build too): `minos` ≤ 12.0
  on every slice of every Mach-O (`tests/mac/minos.sh`), the unpacked bundle's file list equals
  `tests/mac/bundle-files.txt` (`tests/mac/bundle-files.sh`; changing the fixture is a reviewed
  commit, together with a passing tart run), no quarantine in the unpacked zip and no sanitizer
  symbol or runtime (`tests/mac/static-gates.sh`). A failure stops the build.
- **G2, once, before 1.4.0** (mac-core.md §9): the core-based app's switch-over gate. Every
  harness scenario in tart gives 1.3.0's frames; the suites, ASan, UBSan, TSan and the fuzz runs
  pass; P1-P16 within their thresholds; 1.0.0 and 1.3.0 update in-app to 1.4.0 on macOS 26; the
  one-version tests; no quarantine; the maintenance branch green and `release.sh --branch`
  rehearsed once in tart; the full review and the owner's round. It isn't repeated per release.
- **GP, when the bridge or the Prepare helper changes:** before such a change merges, one short
  interleaved performance session against B0 (`bridge/tests/perf.sh gate`, `helper/tests/perf.sh
  gate`, about 20-30 minutes each). A regression blocks the merge.
- **G3, every release, checked by `release.sh`:** `tests/mac/g3.sh run` in tart, one fresh clone
  of the macOS 26 template per scenario, one VM at a time (about 45-60 minutes):
  - from the archived TEST builds of v1.0.0, v1.3.0 and every shipped 1.3.x, the in-app update to
    this release, its version, no quarantine, the watcher registered again, Reinstall and Install,
    the complete uninstall and its trace audit;
  - when the Mac app is carried: the previous release reads the new `payload.json` with no update
    notice, 1.0.0 updates once to the carried app, and About and the report show the release's
    version;
  - always: the new `bootstrap.sh` installs on a clean clone.

  App Management's Allow and the password are emulated. The old apps' buttons are pressed by the G3
  Driver (Accessibility, once the owner has allowed it in the template; `tests/mac/g3.sh smoke`
  says whether a clone has it), else by the owner in the VM's window when asked (at most 10
  minutes per click). A pass writes `build/gates/<commit>/g3-mac.ok`. `release.sh` runs
  `tests/mac/g3.sh check <commit>` before it builds, on main and with `--branch`. It refuses a
  record of another commit, TEST payload or plan, or one with a scenario missing. The
  maintenance branch's own `release.sh` (`maint/1.3`) gets the check once the first real G3
  round has passed (owner decision, 2026-11-12). After publishing, `tests/mac/g3.sh post-publish
  X.Y.Z` checks the real published 1.3.0 updating from `releases/latest` and the real one-liner
  (step 6).
- **Performance, every release** (release.md §6.2): only what a user could notice is gated. A
  slowdown counts only when it is over both 10% and 20 ms (20 MB for memory), on each row's median;
  a row within 25% of its allowance is printed BORDERLINE for a human to judge. Every comparison is
  against a fixed baseline in `gates-archive/` (mac-A for the app, B0 for the bridge and the
  helper), never against the previous release, so small losses add up. The baselines are never
  rebuilt: a missing or mismatched one (`gates-archive/SHA256SUMS`) stops the gate. Which session
  runs depends on the plan:

  | The plan says | Session | Host time |
  |---|---|---|
  | Mac app new | `tests/perf/g0.sh run B.zip` (P1-P16) and the helper's gate (P-H1-P-H5) | about 1-1.5 h |
  | Mac bridge new | `bridge/tests/perf.sh gate` (P-B1-P-B8) | about 20-30 min plus the idle wait |
  | Mac app new, bridge carried | the bridge's P-B1 and P-B5 | about 0.5 h |
  | Mac app carried | `tests/perf/g0.sh run B.zip --scenarios "perf-activation perf-playlist-5000"` (P1, P2, P3, P6, P7.done.full) | about 15 min plus the idle wait |

  A session needs the host to itself, other agents paused, on AC power. The owner starts it.
  `release.sh` doesn't read a performance record yet (release.md §6.2 plans
  `build/gates/<commit>/perf-mac.tsv`). Until it does, a regression blocks the release just the
  same: run `release.sh` only once the session passed, or once the owner has accepted that one
  number in writing (the reason goes into the release notes).

## Checking a release file against a rebuild

Anyone can check a published file against the public source. The trust root stays the signed
`payload.json`: it names every file by SHA-256 (`shasum -a 256 -c SHA256SUMS` checks the
downloaded set), and the rebuild shows that those files come from that source.

1. Clone Gabe-LS/rbstemsplus and check out the tag of the release being checked,
   `v<payload_version>` (its `payload.json`'s `payload_version`; the tag names the public commit
   that `payload.json`'s `commit` names).
2. **A carried file** was built by an earlier commit: check out the one `payload.json` names for
   it instead (`app.commit`, `bridge.commit`; a missing key means its top-level `commit`).
3. **The Mac bridge** (`libonnxruntime.1.18.0.dylib`) is reproducible: `bridge/build.sh` builds the
   same bytes, so its SHA-256 equals `bridge.sha256`.
4. **The Mac app** isn't byte-reproducible (actool writes new fields into `Assets.car` on every
   run, and the signature hashes follow). Build it stamped with the release's `app.version`, as
   `release.sh` does (`RBSTEMSPLUS_APP_VERSION=<app.version> scripts/build-app.sh OUT`, with
   Xcode 26; the first build downloads its pinned sources and checks their SHA-256), then compare
   the two zips with `scripts/bundle-compare.sh --verify OUT/rbstemsplus-app.zip <published zip>`:
   its last line must say "IDENTICAL", or "SAME CODE" (every file identical apart from those
   fields).
5. **Windows files (from 1.5.0)** are all reproducible: rebuild the tag stamped with
   `windows.app.version`, and each file's SHA-256 must equal the published one. A Windows file is
   carried only when its bytes equal this release's build, so this holds for carried files too.

## Windows (from 1.5.0)

Mac and Windows ship in one release with one version (above). The Windows parts start with the
first release that has a `windows` block in `payload.json` (1.5.0). Before that, a release has no
Windows asset, and `release.sh` refuses to run when the latest release's `payload.json` has a
`windows` block (it would drop it, and Windows with it). From 1.5.0 (release.md §6, §7):

- **Assets:** `rbstemsplus-windows.zip`, `rbstemsplus-bridge-win-x64.dll`, `bootstrap.ps1` and
  `NOTICE-windows.txt` join the Mac files. A release that changes only the Mac carries the
  Windows files (no Windows user gets an update or a Stems Cache prompt), and the reverse.
- **Unsigned:** Windows files have no Authenticode signature (owner decision). Changed Windows
  files go to Microsoft's free submission (microsoft.com/wdsi/filesubmission) before each
  release (the owner, about 5 minutes).
- **One more gate record:** `vm-win.ok`, the Windows VM suite, for every release whose Windows app,
  bridge or worker isn't carried, and whenever the `windows` block changes in more than versions
  and commits. The Windows performance sessions follow release.md §6.2's matrix.
- **After publishing:** the real Windows one-liner in a reverted Parallels snapshot.

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
5. **Release notes:** users with Stems Cache get the new list when they update Stems Cache (the
   app asks at launch when its bridge changed).

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
      them the draft's notes, and they are published with the release's files. Wrap them like
      any Markdown here: GitHub keeps every line break of a release's notes as a `<br>`, so
      `release.sh` uploads them unwrapped (`scripts/unwrap-notes.py`: one line per paragraph and
      list item). To change a draft's notes by hand, paste that script's output.
- [ ] Commit everything and push to `origin/main` (the development repository).

### 2. Check locally

```sh
make check
make test
make bridge-test        # outside a sandbox: it attaches disk images
```

`make bridge-test` runs the bridge's tests (chosen locations on exFAT, FAT32, APFS, HFS+ and an
owners-on APFS image included), again with ASan and UBSan, and the fuzz targets. It fails when a
test is skipped (no copy of Pioneer's library, no Rosetta, hdiutil refused): a release needs it
to pass whole. Its last line ("bridge-test: run.sh N passed, 0 failed; SANITIZE=1 ...; fuzz: no
failures (seed ...)") goes into the release commit's message.

- [ ] Check libFLAC's pinned version (`FLAC_VERSION` in `bridge/build.sh`) against its advisories
      (the bridge decodes files anyone who can write a chosen drive may control). The fuzz build
      instruments libFLAC too: UBSan reports signed integer overflows inside libFLAC 1.5.0's
      decoder on corrupt input (`fixed.c:590`, `lpc.c:1083`, restoring samples of a damaged
      frame). Known and non-fatal in the gate: the arithmetic wraps, and the frame's CRC then
      fails, so the entry is refused. A new libFLAC version: check whether they're gone.
- [ ] No downgrades: a 1.1.x app over a 1.2 install ignores `cache_dir` and saves on This Mac;
      the app never offers its own payload's older bridge (payload floor).

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
   The plan comes first: `scripts/release.sh vX.Y.Z --plan` (with the `--mac` and `--branch`
   the release will use) decides which Mac files are carried forward from the latest release
   and writes them to `build/gates/<commit>/plan`; the test build takes that shape (a carried
   app or bridge is the previous release's archived TEST build in
   `gates-archive/tag-vA.B.C/`, signed with the test key archived with it), and the release
   stops if its own decisions differ. Its log is kept for the gate archive (step 6).
   ```sh
   git status                                             # nothing to commit
   scripts/release.sh vX.Y.Z --plan                       # build/gates/<commit>/plan
   scripts/build.sh --test http://192.168.64.1:8765/ --plan build/gates/$(git rev-parse HEAD)/plan 2>&1 | tee build/test-build.log   # into dist-test/
   grep '"commit"' dist-test/payload.json                 # HEAD, no -dirty
   cd dist-test && python3 -m http.server 8765
   ```
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
   - [ ] the previous release installed with Stems Cache, then Update RB Stems Plus: the new app
         asks "Stems Cache update" once it opens (only when the bridge changed); Not Now → asked
         again at the next launch; Update with rekordbox open → asks to quit it; Update →
         password → the light green, `bridge.log` from the new bridge; the next launch asks
         nothing
   - [ ] the previous release installed with Stems Cache, then the new release with the install
         command: the app asks "Stems Cache update" within seconds of opening, once
   - [ ] a chosen drive ejected while rekordbox runs, then connected again: the light says to
         connect it, then turns green within seconds; the same track hits again, no restart
   - [ ] a chosen USB stick unplugged and plugged in again with no load meanwhile: the next load
         of a saved track hits at once ("cache: checked again: using" in `bridge.log`), the light
         stays green
   - [ ] **Prepare** tab (Stems Cache on): a playlist's estimate; Prepare, Pause, Resume, Stop
         (what is done stays done), Prepare again to finish; the **Status** tab says Ready; in
         rekordbox every track of it loads as hits (`bridge.log`: "hit" for every chunk, no
         "miss" line for it)
   - [ ] tracks in Documents or Downloads, and on a drive: macOS asks once per folder; Allow →
         they are prepared; Don't Allow → skipped, and the list says why
   - [ ] rekordbox quit, one prepared track changed (`touch`), rekordbox opened: macOS asks
         whether RB Stems Plus Watcher may send notifications the first time (Allow), then one
         notification, "1 track isn't fully prepared"; with nothing changed, none
   - [ ] each uninstall, then **Uninstall RB Stems Plus…** (in the RB Stems Plus menu)
5. **Trace audit after the complete uninstall:**
   - [ ] rekordbox's file-tree hash equals the one before the install; `codesign --verify
         --deep --strict` passes and `spctl -a -vv` says "Notarized Developer ID"
   - [ ] Pioneer's model is back (its SHA-256, `435c9878…` on the template)
   - [ ] no `~/Library/Application Support/rbstemsplus`, `~/Library/Logs/rbstemsplus`,
         `~/Library/Caches/rbstemsplus`, `/Library/Application Support/rbstemsplus`,
         `~/Library/Preferences/io.github.rbstemsplus.app.plist` (macOS's own preferences of
         the app, e.g. the folder chooser's last folder: removed a few seconds after it quits),
         LaunchAgent plist or background-item record (`sfltool dumpbtm`); kept in the Dock
         before the uninstall, its icon is gone from the Dock after it
   - [ ] no RB Stems Plus in System Settings › Privacy & Security › Files and Folders or App
         Management (when it was allowed there)
   - [ ] with a chosen drive connected: no `RB Stems Plus Cache.noindex` on it, and on exFAT no
         `._RB Stems Plus Cache.noindex` next to where it was; with the drive not connected, the
         final alert names the folder left on it
6. **Faults worth a quick run:** a tampered download (append a byte to the zip on the server:
   the bootstrap must refuse with "checksum mismatch" and change nothing; append a byte to
   `payload.json`, or remove `payload.json.sig`: the bootstrap must refuse with "couldn't be
   verified", and the app's Install must say "Download failed"), wrong password,
   rekordbox running, rekordbox opened during the password sheet (the app must ask to quit it,
   root must exit 5), and a full disk (the app must say "Low disk space" first).
7. **When the root scripts changed:** `tests/root/vm-root-tests.sh` in a throwaway clone (its
   header says how; about 30 minutes).
8. **The gates** on this TEST build ([The gates](#the-gates-from-140)): the G3 tart run
   (`tests/mac/g3.sh run`, its record `build/gates/<commit>/g3-mac.ok`) and the plan's
   performance session. `release.sh` stops without a passing G3 record.

### 4. Preview what would be published

```sh
scripts/release.sh vX.Y.Z --dry-run
```

It needs no key or Terminal, and changes nothing (it fetches the public clone's origin, gh reads
the latest release, and the Mac files are built in a scratch folder).
It makes the checks on `VERSION`, the public clone and the public copy described below, then
shows the commit it would make in the public clone and the public files it would change (every
file, for the first release), and the carry decisions. Read the list: nothing private may be in it. With uncommitted
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
- the carry decisions are the plan's, and G3's tart run passed for this commit
  (`tests/mac/g3.sh check`: `build/gates/<commit>/g3-mac.ok`, made by `tests/mac/g3.sh run` on
  `dist-test/` in this plan's shape, every required scenario passed; release.md §6.1), with
  `--branch` too (this script, from main). The maintenance branch's own release.sh
  (`maint/1.3`) doesn't check it yet: it gets the check (branch `w6/g3-maint`) once the first
  real G3 round has passed (owner decision, 2026-11-12);
- `scripts/build.sh` builds every asset in the public clone, into its `dist/`, with its own
  checks (the clean tree, the commit in `payload.json`, both public keys, the hardened runtime,
  libFLAC's checksum, the model's). The model and libFLAC's tarball come from the development
  repository when the public clone has none;
- no test key ships: this checkout's `build/test-keys` key and every gate-archive tag's are none
  of the public commit's `keys/*.pub.pem` (as points) and aren't in `bootstrap.sh`,
  `bootstrap.ps1` or the app's executable (any point form);
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
- A maintenance release from a branch: `--branch maint/1.3` (HEAD must be on `origin/maint/1.3`).
  Never merge `maint/*` into main; cherry-pick each fix onto main instead.

### 6. Publish

- [ ] Check the draft's notes on GitHub (without `docs/release-notes/X.Y.Z.md` they are only a
      placeholder: write them there).
- [ ] Publish the draft (not as a prerelease, so it becomes `latest`).
- [ ] `tests/mac/g3.sh post-publish X.Y.Z` (an agent, while the owner is there: the guest
      downloads from GitHub and Little Snitch may ask): in fresh clones, the real published 1.3.0
      updates in-app from `releases/latest`, and the real one-liner (no override) installs on a
      clean clone; the record is `build/gates/post-X.Y.Z.txt`. This is the first run of the
      release build's own downloads, and of its signature with the release keys (the VM tests
      used a test build and the test key).
- [ ] Archive the release's TEST build in the gate archive, as `gates-archive/tag-vX.Y.Z/`: a
      later release that carries a Mac or Windows file builds its TEST build from it (`build.sh --test URL
      --plan`), signed with the key pair archived next to it. The key pair is the one that signed
      `dist-test/payload.json`: `build/test-keys/`, or, when this TEST build itself carried a
      file, the previous `tag-vA.B.C/test-keys/` it was signed with (`make clean` deletes
      `build/test-keys/`, so archive it before). Then reseal the archive's `SHA256SUMS`:
      ```sh
      A=gates-archive; . tests/perf/archive.sh && verify     # the archive as it was, first
      a="$A/tag-vX.Y.Z"; mkdir -p "$a/test-keys"
      cp dist-test/rbstemsplus-app.zip dist-test/libonnxruntime.1.18.0.dylib dist-test/payload.json \
         dist-test/payload.json.sig dist-test/bootstrap.sh "$a/"
      for f in rbstemsplus-windows.zip rbstemsplus-bridge-win-x64.dll bootstrap.ps1 NOTICE-windows.txt; do [ ! -f "dist-test/$f" ] || cp "dist-test/$f" "$a/"; done
      cp build/test-build.log "$a/build.log"
      cp build/test-keys/test-key.pem build/test-keys/test.pub.pem "$a/test-keys/"   # or the previous tag's, see above
      /usr/bin/openssl dgst -sha256 -verify "$a/test-keys/test.pub.pem" -signature "$a/payload.json.sig" "$a/payload.json"   # Verified OK
      { echo "tag=vX.Y.Z commit=$(plutil -extract commit raw -o - "$a/payload.json")"
        echo "built=$(stat -f %Sm -t %Y-%m-%dT%H:%M:%S "$a/payload.json")"
        echo "test_url=http://192.168.64.1:8765/"
        echo "swiftc=$(xcrun swiftc --version 2>&1 | head -1)"
        echo "xcode=$(xcodebuild -version | tr '\n' ' ')"
        echo "macos=$(sw_vers -productVersion)"; } > "$a/toolchain.txt"
      seal                                                    # gates-archive/SHA256SUMS anew
      ```
      v1.3.0's is there already (`gates-archive/tag-v1.3.0/`). Copy the archive to the second
      disk afterwards (`tests/perf/b0.sh copy DEST`).

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
